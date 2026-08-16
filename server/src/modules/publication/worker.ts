import { hostname } from 'node:os'

import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import type { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import type {
  NacosClient,
  NacosResourceValue,
  NacosTarget,
} from '../../integrations/nacos/model.js'
import { AppError } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { AuditRepository } from '../audit/repository.js'
import { aggregateReleasePublication, decidePublication } from './decision.js'

interface ClaimCandidateRow extends RowDataPacket {
  job_id: string
  job_public_id: Buffer
  release_id: string
  release_public_id: Buffer
  environment_id: string
  requested_by: string
  nacos_endpoint: string
  nacos_namespace: string
  nacos_tenant: string
  nacos_secret_ref_id: string | null
}

interface ExistingLeaseRow extends RowDataPacket {
  lease_expires_at: string
}

interface PublicationResourceRow extends RowDataPacket {
  id: string
  public_id: Buffer
  project_id: string | null
  kind: 'project_route' | 'project_list' | 'tls_certificates'
  data_id: string
  group_name: string
  operation: 'upsert' | 'remove'
  payload_document_id: string | null
  target_sha256: Buffer | null
  status: 'pending' | 'running' | 'verified' | 'failed' | 'conflict' | 'conflict_after_partial'
  base_exists: 0 | 1
  base_sha256: Buffer | null
  dependency_id: string | null
}

interface StatusRow extends RowDataPacket {
  status: PublicationResourceRow['status']
}

interface AttemptCountRow extends RowDataPacket {
  attempt_count: number
}

interface ProjectLifecycleRow extends RowDataPacket {
  kind: string
  project_id: string | null
  project_public_id: Buffer | null
  domain: string | null
}

export interface PublicationWorkerOptions {
  leaseMillis: number
  owner?: string
}

interface PublicationClaim {
  jobInternalId: string
  jobId: string
  releaseInternalId: string
  releaseId: string
  environmentInternalId: string
  requestedByInternalId: string
  leaseToken: Buffer
  target: NacosTarget
}

function resourceRevision(value: NacosResourceValue): { exists: boolean; sha256: string | null } {
  return { exists: value.exists, sha256: value.sha256 }
}

function errorCode(error: unknown): string {
  return error instanceof AppError ? error.code : 'PUBLICATION_INTERNAL_ERROR'
}

function nullableDigest(value: string | null): Buffer | null {
  return value ? Buffer.from(value, 'hex') : null
}

export class PublicationWorker {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #nacos: NacosClient
  readonly #leaseMillis: number
  readonly #owner: string
  readonly #audit = new AuditRepository()

  constructor(
    pool: DatabasePool,
    documents: DocumentRepository,
    nacos: NacosClient,
    options: PublicationWorkerOptions,
  ) {
    this.#pool = pool
    this.#documents = documents
    this.#nacos = nacos
    this.#leaseMillis = options.leaseMillis
    this.#owner = options.owner ?? `${hostname()}:${process.pid}`
  }

  async runOnce(): Promise<boolean> {
    const claim = await this.claim()
    if (!claim) return false
    try {
      await this.publish(claim)
    } catch (error) {
      await this.failClaim(claim, errorCode(error))
    }
    return true
  }

  private async claim(): Promise<PublicationClaim | null> {
    let claim: PublicationClaim | null = null
    await withTransaction(this.#pool, async (transaction) => {
      const [rows] = await transaction.execute<ClaimCandidateRow[]>(
        `SELECT
           job.id AS job_id, job.public_id AS job_public_id,
           rel.id AS release_id, rel.public_id AS release_public_id, rel.environment_id,
           job.requested_by,
           env.nacos_endpoint, env.nacos_namespace, env.nacos_tenant, env.nacos_secret_ref_id
         FROM publication_jobs job
         INNER JOIN releases rel ON rel.id = job.release_id
         INNER JOIN environments env ON env.id = rel.environment_id
         WHERE job.state IN ('queued', 'retry')
           AND job.next_run_at <= CURRENT_TIMESTAMP(6)
           AND rel.status = 'queued'
         ORDER BY job.next_run_at, job.id
         LIMIT 1
         FOR UPDATE SKIP LOCKED`,
      )
      const row = rows[0]
      if (!row) return

      const [leaseRows] = await transaction.execute<ExistingLeaseRow[]>(
        `SELECT lease_expires_at
         FROM environment_publish_leases
         WHERE environment_id = ?
         FOR UPDATE`,
        [row.environment_id],
      )
      if (leaseRows[0]) {
        const [deleteResult] = await transaction.execute<ResultSetHeader>(
          `DELETE FROM environment_publish_leases
           WHERE environment_id = ? AND lease_expires_at <= CURRENT_TIMESTAMP(6)`,
          [row.environment_id],
        )
        if (deleteResult.affectedRows === 0) return
      }

      const leaseToken = publicIdToBuffer(createPublicId())
      const leaseMicros = this.#leaseMillis * 1_000
      await transaction.execute(
        `INSERT INTO environment_publish_leases
          (environment_id, release_id, publication_job_id, lease_owner, lease_token,
           lease_expires_at, updated_at)
         VALUES (?, ?, ?, ?, ?,
                 DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND), CURRENT_TIMESTAMP(6))`,
        [row.environment_id, row.release_id, row.job_id, this.#owner, leaseToken, leaseMicros],
      )
      await transaction.execute(
        `UPDATE publication_jobs
         SET state = 'running', lease_owner = ?, lease_token = ?,
             lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND),
             heartbeat_at = CURRENT_TIMESTAMP(6), attempt_count = attempt_count + 1
         WHERE id = ?`,
        [this.#owner, leaseToken, leaseMicros, row.job_id],
      )
      await transaction.execute(
        `UPDATE releases
         SET status = 'publishing', publish_started_at = COALESCE(publish_started_at, CURRENT_TIMESTAMP(6)),
             lock_version = lock_version + 1
         WHERE id = ? AND status = 'queued'`,
        [row.release_id],
      )
      claim = {
        jobInternalId: row.job_id,
        jobId: bufferToPublicId(row.job_public_id),
        releaseInternalId: row.release_id,
        releaseId: bufferToPublicId(row.release_public_id),
        environmentInternalId: row.environment_id,
        requestedByInternalId: row.requested_by,
        leaseToken,
        target: {
          endpoint: row.nacos_endpoint,
          namespace: row.nacos_namespace,
          tenant: row.nacos_tenant,
          credentialConfigured: row.nacos_secret_ref_id !== null,
        },
      }
    })
    return claim
  }

  private async publish(claim: PublicationClaim): Promise<void> {
    const [resources] = await this.#pool.execute<PublicationResourceRow[]>(
      `SELECT
         rr.id, rr.public_id, rr.project_id, rr.kind, rr.data_id, rr.group_name,
         rr.operation, rr.payload_document_id, rr.target_sha256, rr.status,
         observation.resource_exists AS base_exists, observation.sha256 AS base_sha256,
         dependency.id AS dependency_id
       FROM release_resources rr
       INNER JOIN nacos_resource_observations observation ON observation.id = rr.base_observation_id
       LEFT JOIN release_resource_dependencies edge ON edge.resource_id = rr.id
       LEFT JOIN release_resources dependency ON dependency.id = edge.depends_on_resource_id
       WHERE rr.release_id = ? AND rr.required_resource = TRUE
       ORDER BY rr.publish_order, rr.id`,
      [claim.releaseInternalId],
    )

    let changedTargetEnvironment = false
    for (const resource of resources) {
      await this.heartbeat(claim)
      if (resource.status === 'verified') continue
      if (resource.dependency_id && !(await this.dependencyVerified(resource.dependency_id))) {
        await this.finishResourceFailure(
          claim,
          resource,
          'failed',
          'PUBLICATION_DEPENDENCY_NOT_VERIFIED',
          null,
          null,
        )
        continue
      }
      const changed = await this.publishResource(claim, resource, changedTargetEnvironment)
      changedTargetEnvironment ||= changed
    }
    await this.finishClaim(claim, changedTargetEnvironment)
  }

  private async dependencyVerified(resourceInternalId: string): Promise<boolean> {
    const [rows] = await this.#pool.execute<StatusRow[]>(
      'SELECT status FROM release_resources WHERE id = ?',
      [resourceInternalId],
    )
    return rows[0]?.status === 'verified'
  }

  private async publishResource(
    claim: PublicationClaim,
    resource: PublicationResourceRow,
    previouslyWritten: boolean,
  ): Promise<boolean> {
    const attempt = await this.startAttempt(claim, resource)
    let before: NacosResourceValue | null = null
    let wrote = false
    try {
      before = await this.#nacos.read(claim.target, resource.data_id, resource.group_name)
      const targetSha256 = resource.target_sha256?.toString('hex') ?? null
      const decision = decidePublication({
        operation: resource.operation,
        before: resourceRevision(before),
        base: {
          exists: resource.base_exists === 1,
          sha256: resource.base_sha256?.toString('hex') ?? null,
        },
        targetSha256,
        previouslyWritten,
      })
      if (decision === 'conflict' || decision === 'conflict_after_partial') {
        await this.finishResourceFailure(
          claim,
          resource,
          decision,
          decision === 'conflict' ? 'NACOS_BASE_CONFLICT' : 'NACOS_BASE_CONFLICT_AFTER_PARTIAL',
          before,
          attempt,
        )
        return false
      }

      if (decision === 'write') {
        if (!resource.payload_document_id) {
          throw new Error('Release resource payload is missing')
        }
        const payload = await this.#documents.decryptByInternalId(
          this.#pool,
          resource.payload_document_id,
        )
        if (!payload) throw new Error('Release resource payload could not be loaded')
        await this.#nacos.write(
          claim.target,
          resource.data_id,
          resource.group_name,
          payload.toString('utf8'),
          resource.kind === 'project_list' ? 'text' : 'json',
        )
        wrote = true
      } else if (decision === 'remove') {
        throw new Error('Nacos resource removal is not implemented')
      }

      const after = await this.#nacos.read(claim.target, resource.data_id, resource.group_name)
      const verified =
        resource.operation === 'upsert'
          ? after.exists && after.sha256 === targetSha256
          : !after.exists
      if (!verified) {
        await this.finishResourceFailure(
          claim,
          resource,
          'failed',
          'NACOS_READBACK_MISMATCH',
          before,
          attempt,
          after,
        )
        return wrote
      }
      await this.#pool.execute(
        `UPDATE publication_attempts
         SET result = ?, before_exists = ?, before_nacos_md5 = ?, before_sha256 = ?,
             after_nacos_md5 = ?, after_sha256 = ?, finished_at = CURRENT_TIMESTAMP(6)
         WHERE id = ?`,
        [
          wrote ? 'written_and_verified' : 'already_at_target',
          before.exists,
          nullableDigest(before.md5),
          nullableDigest(before.sha256),
          nullableDigest(after.md5),
          nullableDigest(after.sha256),
          attempt,
        ],
      )
      await this.#pool.execute(
        `UPDATE release_resources
         SET status = 'verified', verified_nacos_md5 = ?, verified_sha256 = ?,
             verified_at = CURRENT_TIMESTAMP(6), lock_version = lock_version + 1
         WHERE id = ?`,
        [nullableDigest(after.md5), nullableDigest(after.sha256), resource.id],
      )
      return wrote
    } catch (error) {
      await this.finishResourceFailure(claim, resource, 'failed', errorCode(error), before, attempt)
      return wrote
    }
  }

  private async startAttempt(
    claim: PublicationClaim,
    resource: PublicationResourceRow,
  ): Promise<string> {
    const [counts] = await this.#pool.execute<AttemptCountRow[]>(
      `SELECT COUNT(*) AS attempt_count
       FROM publication_attempts
       WHERE release_resource_id = ?`,
      [resource.id],
    )
    const attemptNo = (counts[0]?.attempt_count ?? 0) + 1
    const [result] = await this.#pool.execute<ResultSetHeader>(
      `INSERT INTO publication_attempts
        (public_id, publication_job_id, release_resource_id, attempt_no, idempotency_key,
         result, started_at)
       VALUES (?, ?, ?, ?, ?, 'running', CURRENT_TIMESTAMP(6))`,
      [
        publicIdToBuffer(createPublicId()),
        claim.jobInternalId,
        resource.id,
        attemptNo,
        `${claim.jobId}:${bufferToPublicId(resource.public_id)}:${attemptNo}`,
      ],
    )
    await this.#pool.execute(
      `UPDATE release_resources
       SET status = 'running', lock_version = lock_version + 1
       WHERE id = ?`,
      [resource.id],
    )
    return result.insertId.toString()
  }

  private async finishResourceFailure(
    claim: PublicationClaim,
    resource: PublicationResourceRow,
    status: 'failed' | 'conflict' | 'conflict_after_partial',
    code: string,
    before: NacosResourceValue | null,
    attemptInternalId: string | null,
    after: NacosResourceValue | null = null,
  ): Promise<void> {
    let attempt = attemptInternalId
    if (!attempt) attempt = await this.startAttempt(claim, resource)
    await this.#pool.execute(
      `UPDATE publication_attempts
       SET result = ?, before_exists = ?, before_nacos_md5 = ?, before_sha256 = ?,
           after_nacos_md5 = ?, after_sha256 = ?, error_code = ?,
           error_detail_json = JSON_OBJECT('code', ?), finished_at = CURRENT_TIMESTAMP(6)
       WHERE id = ?`,
      [
        status,
        before?.exists ?? null,
        nullableDigest(before?.md5 ?? null),
        nullableDigest(before?.sha256 ?? null),
        nullableDigest(after?.md5 ?? null),
        nullableDigest(after?.sha256 ?? null),
        code,
        code,
        attempt,
      ],
    )
    await this.#pool.execute(
      `UPDATE release_resources
       SET status = ?, lock_version = lock_version + 1
       WHERE id = ?`,
      [status, resource.id],
    )
  }

  private async heartbeat(claim: PublicationClaim): Promise<void> {
    const leaseMicros = this.#leaseMillis * 1_000
    const [result] = await this.#pool.execute<ResultSetHeader>(
      `UPDATE publication_jobs
       SET heartbeat_at = CURRENT_TIMESTAMP(6),
           lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND)
       WHERE id = ? AND lease_token = ? AND state = 'running'`,
      [leaseMicros, claim.jobInternalId, claim.leaseToken],
    )
    if (result.affectedRows !== 1) throw new Error('Publication job lease was lost')
    await this.#pool.execute(
      `UPDATE environment_publish_leases
       SET lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND),
           updated_at = CURRENT_TIMESTAMP(6)
       WHERE environment_id = ? AND lease_token = ?`,
      [leaseMicros, claim.environmentInternalId, claim.leaseToken],
    )
  }

  private async finishClaim(
    claim: PublicationClaim,
    changedTargetEnvironment: boolean,
  ): Promise<void> {
    const [rows] = await this.#pool.execute<StatusRow[]>(
      `SELECT status
       FROM release_resources
       WHERE release_id = ? AND required_resource = TRUE`,
      [claim.releaseInternalId],
    )
    const status = aggregateReleasePublication(
      rows.map((row) => row.status),
      changedTargetEnvironment,
    )
    if (status === 'publishing') throw new Error('Publication did not reach a terminal state')

    await withTransaction(this.#pool, async (transaction) => {
      const jobState = status === 'published' ? 'succeeded' : 'failed'
      await transaction.execute(
        `UPDATE releases
         SET status = ?, published_at = IF(? = 'published', CURRENT_TIMESTAMP(6), published_at),
             lock_version = lock_version + 1
         WHERE id = ? AND status = 'publishing'`,
        [status, status, claim.releaseInternalId],
      )
      await transaction.execute(
        `UPDATE publication_jobs
         SET state = ?, finished_at = CURRENT_TIMESTAMP(6), lease_owner = NULL,
             lease_token = NULL, lease_expires_at = NULL, heartbeat_at = CURRENT_TIMESTAMP(6),
             last_error_code = ?
         WHERE id = ? AND lease_token = ?`,
        [
          jobState,
          status === 'published' ? null : 'PUBLICATION_NOT_FULLY_VERIFIED',
          claim.jobInternalId,
          claim.leaseToken,
        ],
      )
      if (status === 'published') {
        await transaction.execute(
          `INSERT INTO release_activation_targets (release_id, instance_id, required_target)
           SELECT ?, instance_record.id, TRUE
           FROM access_server_instances instance_record
           WHERE instance_record.environment_id = ? AND instance_record.enabled = TRUE
           ON DUPLICATE KEY UPDATE required_target = TRUE`,
          [claim.releaseInternalId, claim.environmentInternalId],
        )
        await transaction.execute(
          `UPDATE releases previous
           INNER JOIN release_items previous_item
             ON previous_item.release_id = previous.id AND previous_item.kind = 'project_route'
           INNER JOIN release_items current_item
             ON current_item.release_id = ? AND current_item.kind = 'project_route'
           SET previous.status = 'superseded', previous.lock_version = previous.lock_version + 1
           WHERE previous.id <> ? AND previous.status = 'published'
             AND previous_item.project_id = current_item.project_id`,
          [claim.releaseInternalId, claim.releaseInternalId],
        )
        await transaction.execute(
          `UPDATE releases previous
           INNER JOIN releases current ON current.id = ?
           SET previous.status = 'superseded', previous.lock_version = previous.lock_version + 1
           WHERE previous.id <> ? AND previous.status = 'published'
             AND previous.kind = 'tls_certificates'
             AND current.kind = 'tls_certificates'
             AND previous.environment_id = current.environment_id`,
          [claim.releaseInternalId, claim.releaseInternalId],
        )
        const [lifecycleRows] = await transaction.execute<ProjectLifecycleRow[]>(
          `SELECT rel.kind, project_record.id AS project_id,
                  project_record.public_id AS project_public_id, project_record.name AS domain
           FROM releases rel
           LEFT JOIN release_items item
             ON item.release_id = rel.id AND item.kind = 'project_decommission'
           LEFT JOIN projects project_record ON project_record.id = item.project_id
           WHERE rel.id = ?
           LIMIT 1`,
          [claim.releaseInternalId],
        )
        const lifecycle = lifecycleRows[0]
        if (
          lifecycle?.kind === 'project_decommission' &&
          lifecycle.project_id &&
          lifecycle.project_public_id &&
          lifecycle.domain
        ) {
          await transaction.execute(
            `UPDATE releases previous
             INNER JOIN release_items previous_item
               ON previous_item.release_id = previous.id
              AND previous_item.kind IN ('project_route', 'project_decommission')
             SET previous.status = 'superseded',
                 previous.lock_version = previous.lock_version + 1
             WHERE previous.id <> ? AND previous.status = 'published'
               AND previous_item.project_id = ?`,
            [claim.releaseInternalId, lifecycle.project_id],
          )
          const [archived] = await transaction.execute<ResultSetHeader>(
            `UPDATE projects
             SET status = 'archived', archived_at = CURRENT_TIMESTAMP(6),
                 updated_at = CURRENT_TIMESTAMP(6), lock_version = lock_version + 1
             WHERE id = ? AND status = 'decommissioning' AND archived_at IS NULL`,
            [lifecycle.project_id],
          )
          if (archived.affectedRows === 1) {
            await this.#audit.append(transaction, {
              environmentInternalId: claim.environmentInternalId,
              actorInternalId: claim.requestedByInternalId,
              eventType: 'project.archived',
              targetType: 'project',
              targetPublicId: bufferToPublicId(lifecycle.project_public_id),
              requestId: claim.jobId,
              result: 'success',
              summary: {
                releaseId: claim.releaseId,
                domain: lifecycle.domain,
                activationStatus: 'unknown',
              },
            })
          }
        }
      }
      await this.#audit.append(transaction, {
        environmentInternalId: claim.environmentInternalId,
        actorInternalId: claim.requestedByInternalId,
        eventType: 'publication.completed',
        targetType: 'release',
        targetPublicId: claim.releaseId,
        requestId: claim.jobId,
        result: status === 'published' ? 'success' : 'failure',
        summary: { status, changedTargetEnvironment },
      })
      await transaction.execute(
        `DELETE FROM environment_publish_leases
         WHERE environment_id = ? AND lease_token = ?`,
        [claim.environmentInternalId, claim.leaseToken],
      )
    })
  }

  private async failClaim(claim: PublicationClaim, code: string): Promise<void> {
    await withTransaction(this.#pool, async (transaction) => {
      await transaction.execute(
        `UPDATE releases
         SET status = CASE
               WHEN EXISTS (
                 SELECT 1 FROM release_resources rr
                 WHERE rr.release_id = releases.id AND rr.status = 'verified'
               ) THEN 'partially_published'
               ELSE 'publish_failed'
             END,
             lock_version = lock_version + 1
         WHERE id = ? AND status = 'publishing'`,
        [claim.releaseInternalId],
      )
      await transaction.execute(
        `UPDATE publication_jobs
         SET state = 'failed', last_error_code = ?, finished_at = CURRENT_TIMESTAMP(6),
             lease_owner = NULL, lease_token = NULL, lease_expires_at = NULL
         WHERE id = ? AND lease_token = ?`,
        [code, claim.jobInternalId, claim.leaseToken],
      )
      await this.#audit.append(transaction, {
        environmentInternalId: claim.environmentInternalId,
        actorInternalId: claim.requestedByInternalId,
        eventType: 'publication.completed',
        targetType: 'release',
        targetPublicId: claim.releaseId,
        requestId: claim.jobId,
        result: 'failure',
        summary: { status: 'publish_failed', errorCode: code },
      })
      await transaction.execute(
        `DELETE FROM environment_publish_leases
         WHERE environment_id = ? AND lease_token = ?`,
        [claim.environmentInternalId, claim.leaseToken],
      )
    })
  }
}
