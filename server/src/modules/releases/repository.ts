import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import type { NacosResourceValue } from '../../integrations/nacos/model.js'
import { conflict, notFound } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type { ProjectIdentityRow } from '../projects/repository.js'
import type { StoredConfigurationVersion } from '../versions/repository.js'
import type { ProjectReleaseView, QueuePublicationResult, ReleaseResourceView } from './model.js'
import type { ReleaseStatus } from './state.js'

interface ReleaseRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  sequence_no: string
  project_public_id: Buffer
  kind: 'project_route' | 'project_decommission'
  title: string
  description: string
  status: ReleaseStatus
  source_version_public_id: Buffer | null
  source_version_no: number | null
  source_relation: 'current' | 'historical' | null
  current_version_public_id: Buffer | null
  current_version_no: number | null
  allocated_project_version: number | null
  source_model_sha256: Buffer
  wire_sha256: Buffer | null
  native_validator_contract: number | null
  native_validator_revision: string | null
  compiler_revision: string | null
  validation_errors_json: unknown
  publication_job_public_id: Buffer | null
  publication_job_state: string | null
  created_at: string
  published_at: string | null
}

interface ResourceRow extends RowDataPacket {
  public_id: Buffer
  kind: ReleaseResourceView['kind']
  data_id: string
  group_name: string
  operation: ReleaseResourceView['operation']
  status: ReleaseResourceView['status']
  target_sha256: Buffer | null
  verified_sha256: Buffer | null
  verified_at: string | null
}

interface LockedDraftRow extends RowDataPacket {
  id: string
  current_revision_no: number
}

interface CurrentRevisionRow extends RowDataPacket {
  id: string
  public_id: Buffer
  revision_no: number
}

interface ExistingReleaseRow extends RowDataPacket {
  public_id: Buffer
  request_sha256: Buffer
}

export interface BeginReleaseInput {
  actor: Actor
  project: ProjectIdentityRow
  source: StoredConfigurationVersion
  expectedCurrentVersionId: string
  title: string
  description: string
  idempotencyKey: string
  requestSha256: Buffer
  compilerRevision: string
  validatorContractVersion: number
  validatorRevision: string
  requestId: string
}

export interface BeginReleaseResult {
  release: ProjectReleaseView
  replay: boolean
}

export interface BeginDecommissionReleaseInput {
  actor: Actor
  project: ProjectIdentityRow
  expectedLockVersion: string
  reason: string
  idempotencyKey: string
  requestSha256: Buffer
  requestId: string
  planText: string
  dataId: string
  group: string
  base: NacosResourceValue
  targetContent: string
}

export interface PreparedReleaseResource {
  kind: 'project_route' | 'project_list'
  dataId: string
  group: string
  contentType: 'json' | 'text'
  targetContent: string
  base: NacosResourceValue
  publishOrder: number
  dependsOnKind?: 'project_route'
}

function parseJsonArray(value: unknown): readonly unknown[] {
  const parsed: unknown = typeof value === 'string' ? JSON.parse(value) : value
  return Array.isArray(parsed) ? parsed : []
}

const selectRelease = `
  SELECT
    rel.id AS internal_id, rel.public_id, rel.sequence_no,
    p.public_id AS project_public_id,
    rel.kind, rel.title, rel.description, rel.status,
    source.public_id AS source_version_public_id, source.revision_no AS source_version_no,
    ri.source_relation,
    current_at_creation.public_id AS current_version_public_id,
    current_at_creation.revision_no AS current_version_no,
    ri.allocated_project_version,
    source_doc.plaintext_sha256 AS source_model_sha256,
    (
      SELECT rr.target_sha256
      FROM release_resources rr
      WHERE rr.release_id = rel.id AND rr.kind = 'project_route'
      LIMIT 1
    ) AS wire_sha256,
    rel.native_validator_contract, rel.native_validator_revision,
    rel.compiler_revision, rel.validation_errors_json,
    job.public_id AS publication_job_public_id, job.state AS publication_job_state,
    rel.created_at, rel.published_at
  FROM releases rel
  INNER JOIN release_items ri
    ON ri.release_id = rel.id AND ri.kind IN ('project_route', 'project_decommission')
  INNER JOIN projects p ON p.id = ri.project_id
  LEFT JOIN draft_revisions source ON source.id = ri.draft_revision_id
  INNER JOIN config_documents source_doc ON source_doc.id = ri.model_document_id
  LEFT JOIN draft_revisions current_at_creation
    ON current_at_creation.id = rel.current_revision_id_at_creation
  LEFT JOIN publication_jobs job ON job.release_id = rel.id
`

function resourceView(row: ResourceRow): ReleaseResourceView {
  return {
    id: bufferToPublicId(row.public_id),
    kind: row.kind,
    dataId: row.data_id,
    group: row.group_name,
    operation: row.operation,
    status: row.status,
    targetSha256: row.target_sha256?.toString('hex') ?? null,
    verifiedSha256: row.verified_sha256?.toString('hex') ?? null,
    verifiedAt: row.verified_at ? mysqlDateTimeToRfc3339(row.verified_at) : null,
  }
}

export class ReleaseRepository {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, documents: DocumentRepository, audit = new AuditRepository()) {
    this.#pool = pool
    this.#documents = documents
    this.#audit = audit
  }

  async beginCreate(input: BeginReleaseInput): Promise<BeginReleaseResult> {
    let releasePublicId = createPublicId()
    let replay = false

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [existingRows] = await transaction.execute<ExistingReleaseRow[]>(
          `SELECT public_id, request_sha256
           FROM releases
           WHERE environment_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [input.project.environment_id, input.actor.internalId, input.idempotencyKey],
        )
        const existing = existingRows[0]
        if (existing) {
          if (!existing.request_sha256.equals(input.requestSha256)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with a different release request',
            )
          }
          releasePublicId = bufferToPublicId(existing.public_id)
          replay = true
          return
        }

        const [projectRows] = await transaction.execute<
          (RowDataPacket & { status: ProjectIdentityRow['status'] })[]
        >('SELECT status FROM projects WHERE id = ? FOR UPDATE', [input.project.id])
        if (projectRows[0]?.status !== 'active') {
          throw conflict('PROJECT_NOT_ACTIVE', 'Only an active Project can create a route Release')
        }
        const [lockedExistingRows] = await transaction.execute<ExistingReleaseRow[]>(
          `SELECT public_id, request_sha256
           FROM releases
           WHERE environment_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [input.project.environment_id, input.actor.internalId, input.idempotencyKey],
        )
        const lockedExisting = lockedExistingRows[0]
        if (lockedExisting) {
          if (!lockedExisting.request_sha256.equals(input.requestSha256)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with a different release request',
            )
          }
          releasePublicId = bufferToPublicId(lockedExisting.public_id)
          replay = true
          return
        }

        const [draftRows] = await transaction.execute<LockedDraftRow[]>(
          `SELECT id, current_revision_no
           FROM drafts
           WHERE project_id = ? AND archived_at IS NULL
           FOR UPDATE`,
          [input.project.id],
        )
        const draft = draftRows[0]
        if (!draft) throw notFound('Configuration workspace')
        const [currentRows] = await transaction.execute<CurrentRevisionRow[]>(
          `SELECT id, public_id, revision_no
           FROM draft_revisions
           WHERE draft_id = ? AND revision_no = ?`,
          [draft.id, draft.current_revision_no],
        )
        const current = currentRows[0]
        if (!current || bufferToPublicId(current.public_id) !== input.expectedCurrentVersionId) {
          throw conflict(
            'CONFIG_VERSION_CONFLICT',
            'The current configuration version changed; review the release again',
          )
        }
        const [sourceRows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
          'SELECT id FROM draft_revisions WHERE id = ? AND draft_id = ?',
          [input.source.internalId, draft.id],
        )
        if (!sourceRows[0]) throw notFound('Configuration version')
        const relation = input.source.internalId === current.id ? 'current' : 'historical'

        const [environmentRows] = await transaction.execute<
          (RowDataPacket & { last_release_sequence: string })[]
        >('SELECT last_release_sequence FROM environments WHERE id = ? FOR UPDATE', [
          input.project.environment_id,
        ])
        const environment = environmentRows[0]
        if (!environment) throw notFound('Workspace')
        const nextSequence = BigInt(environment.last_release_sequence) + 1n

        const [counterRows] = await transaction.execute<
          (RowDataPacket & { last_allocated_version: number })[]
        >(
          'SELECT last_allocated_version FROM project_version_counters WHERE project_id = ? FOR UPDATE',
          [input.project.id],
        )
        const counter = counterRows[0]
        if (!counter) throw new Error('Project wire version counter is missing')
        if (counter.last_allocated_version >= 2_147_483_647) {
          throw conflict('WIRE_VERSION_EXHAUSTED', 'The project wire version range is exhausted')
        }
        const wireVersion = counter.last_allocated_version + 1

        await transaction.execute(
          'UPDATE environments SET last_release_sequence = ? WHERE id = ?',
          [nextSequence.toString(), input.project.environment_id],
        )
        await transaction.execute(
          `UPDATE project_version_counters
           SET last_allocated_version = ?, updated_at = CURRENT_TIMESTAMP(6)
           WHERE project_id = ?`,
          [wireVersion, input.project.id],
        )
        const [releaseResult] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO releases
            (public_id, environment_id, sequence_no, kind, status, title, description,
             compiler_revision, current_revision_id_at_creation, idempotency_key,
             request_sha256, validation_errors_json, native_validator_contract,
             native_validator_revision, created_by)
           VALUES (?, ?, ?, 'project_route', 'validating', ?, ?, ?, ?, ?, ?, JSON_ARRAY(), ?, ?, ?)`,
          [
            publicIdToBuffer(releasePublicId),
            input.project.environment_id,
            nextSequence.toString(),
            input.title,
            input.description,
            input.compilerRevision,
            current.id,
            input.idempotencyKey,
            input.requestSha256,
            input.validatorContractVersion,
            input.validatorRevision,
            input.actor.internalId,
          ],
        )
        const releaseInternalId = releaseResult.insertId.toString()
        await transaction.execute(
          `INSERT INTO release_items
            (public_id, release_id, project_id, kind, draft_revision_id, source_relation,
             model_document_id, allocated_project_version, change_kind, diff_summary_json)
           VALUES (?, ?, ?, 'project_route', ?, ?, ?, ?, 'update', JSON_OBJECT())`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            input.project.id,
            input.source.internalId,
            relation,
            input.source.modelDocumentInternalId,
            wireVersion,
          ],
        )
        await this.#audit.append(transaction, {
          environmentInternalId: input.project.environment_id,
          actorInternalId: input.actor.internalId,
          eventType:
            relation === 'current'
              ? 'release.created_from_current'
              : 'release.created_from_history',
          targetType: 'release',
          targetPublicId: releasePublicId,
          requestId: input.requestId,
          result: 'success',
          summary: {
            projectId: input.source.projectId,
            sourceVersionId: input.source.id,
            sourceVersionNumber: input.source.number,
            currentVersionId: bufferToPublicId(current.public_id),
            currentVersionNumber: current.revision_no,
            relation,
            allocatedWireVersion: wireVersion,
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const release = await this.findByPublicId(releasePublicId)
    if (!release) throw new Error('Release could not be reloaded after creation')
    return { release, replay }
  }

  async beginDecommission(input: BeginDecommissionReleaseInput): Promise<BeginReleaseResult> {
    let releasePublicId = createPublicId()
    let replay = false
    const planEncrypted = this.#documents.encrypt(Buffer.from(input.planText, 'utf8'))
    const targetEncrypted = this.#documents.encrypt(Buffer.from(input.targetContent, 'utf8'))
    const baseEncrypted =
      input.base.exists && input.base.content !== null
        ? this.#documents.encrypt(Buffer.from(input.base.content, 'utf8'))
        : null

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [existingRows] = await transaction.execute<ExistingReleaseRow[]>(
          `SELECT public_id, request_sha256
           FROM releases
           WHERE environment_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [input.project.environment_id, input.actor.internalId, input.idempotencyKey],
        )
        const existing = existingRows[0]
        if (existing) {
          if (!existing.request_sha256.equals(input.requestSha256)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with a different release request',
            )
          }
          releasePublicId = bufferToPublicId(existing.public_id)
          replay = true
          return
        }

        const [projectRows] = await transaction.execute<
          (RowDataPacket & {
            status: ProjectIdentityRow['status']
            lock_version: string
            archived_at: string | null
          })[]
        >(
          `SELECT status, lock_version, archived_at
           FROM projects
           WHERE id = ?
           FOR UPDATE`,
          [input.project.id],
        )
        const project = projectRows[0]
        if (!project || project.status === 'archived' || project.archived_at !== null) {
          throw conflict('PROJECT_ARCHIVED', 'An archived Project cannot be decommissioned again')
        }
        const [lockedExistingRows] = await transaction.execute<ExistingReleaseRow[]>(
          `SELECT public_id, request_sha256
           FROM releases
           WHERE environment_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [input.project.environment_id, input.actor.internalId, input.idempotencyKey],
        )
        const lockedExisting = lockedExistingRows[0]
        if (lockedExisting) {
          if (!lockedExisting.request_sha256.equals(input.requestSha256)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with a different release request',
            )
          }
          releasePublicId = bufferToPublicId(lockedExisting.public_id)
          replay = true
          return
        }
        if (project.lock_version !== input.expectedLockVersion) {
          throw conflict(
            'PROJECT_VERSION_CONFLICT',
            'The Project changed; refresh Settings and confirm decommissioning again',
          )
        }

        const [activeReleaseRows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
          `SELECT rel.id
           FROM releases rel
           INNER JOIN release_items item ON item.release_id = rel.id
           WHERE item.project_id = ?
             AND rel.status IN ('creating', 'validating', 'ready', 'queued', 'publishing')
           LIMIT 1`,
          [input.project.id],
        )
        if (activeReleaseRows[0]) {
          throw conflict(
            'PROJECT_DECOMMISSION_IN_PROGRESS',
            'The Project already has a non-terminal Release',
          )
        }

        const [environmentRows] = await transaction.execute<
          (RowDataPacket & { last_release_sequence: string })[]
        >('SELECT last_release_sequence FROM environments WHERE id = ? FOR UPDATE', [
          input.project.environment_id,
        ])
        const environment = environmentRows[0]
        if (!environment) throw notFound('Workspace')
        const nextSequence = BigInt(environment.last_release_sequence) + 1n

        const [currentRows] = await transaction.execute<CurrentRevisionRow[]>(
          `SELECT revision.id, revision.public_id, revision.revision_no
           FROM drafts draft
           INNER JOIN draft_revisions revision
             ON revision.draft_id = draft.id
            AND revision.revision_no = draft.current_revision_no
           WHERE draft.project_id = ? AND draft.archived_at IS NULL
           LIMIT 1`,
          [input.project.id],
        )
        const current = currentRows[0] ?? null

        const planDocument = await this.#documents.insert(transaction, {
          environmentInternalId: input.project.environment_id,
          purpose: 'release_model',
          contentType: 'application/json',
          schemaVersion: 1,
          encrypted: planEncrypted,
        })
        let baseDocumentId: string | null = null
        if (baseEncrypted) {
          const baseDocument = await this.#documents.insert(transaction, {
            environmentInternalId: input.project.environment_id,
            purpose: 'nacos_observation',
            contentType: 'text/plain',
            schemaVersion: null,
            encrypted: baseEncrypted,
          })
          baseDocumentId = baseDocument.internalId
        }
        const [observation] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO nacos_resource_observations
            (public_id, environment_id, data_id, group_name, resource_exists,
             payload_document_id, nacos_md5, sha256, source, client_result, fetched_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'release_preflight', 'success', CURRENT_TIMESTAMP(6))`,
          [
            publicIdToBuffer(createPublicId()),
            input.project.environment_id,
            input.dataId,
            input.group,
            input.base.exists,
            baseDocumentId,
            input.base.md5 ? Buffer.from(input.base.md5, 'hex') : null,
            input.base.sha256 ? Buffer.from(input.base.sha256, 'hex') : null,
          ],
        )
        const targetDocument = await this.#documents.insert(transaction, {
          environmentInternalId: input.project.environment_id,
          purpose: 'release_payload',
          contentType: 'text/plain',
          schemaVersion: null,
          encrypted: targetEncrypted,
        })

        await transaction.execute(
          'UPDATE environments SET last_release_sequence = ? WHERE id = ?',
          [nextSequence.toString(), input.project.environment_id],
        )
        const [releaseResult] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO releases
            (public_id, environment_id, sequence_no, kind, status, title, description,
             compiler_revision, current_revision_id_at_creation, idempotency_key,
             request_sha256, validation_errors_json, native_validator_contract,
             native_validator_revision, created_by, ready_at)
           VALUES (?, ?, ?, 'project_decommission', 'ready', ?, ?, 'project-decommission-v1',
                   ?, ?, ?, JSON_ARRAY(), NULL, NULL, ?, CURRENT_TIMESTAMP(6))`,
          [
            publicIdToBuffer(releasePublicId),
            input.project.environment_id,
            nextSequence.toString(),
            `下线 ${input.project.name}`,
            input.reason,
            current?.id ?? null,
            input.idempotencyKey,
            input.requestSha256,
            input.actor.internalId,
          ],
        )
        const releaseInternalId = releaseResult.insertId.toString()
        await transaction.execute(
          `INSERT INTO release_items
            (public_id, release_id, project_id, kind, draft_revision_id, source_relation,
             model_document_id, allocated_project_version, change_kind, diff_summary_json)
           VALUES (?, ?, ?, 'project_decommission', NULL, NULL, ?, NULL, 'remove', ?)`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            input.project.id,
            planDocument.internalId,
            JSON.stringify({ domain: input.project.name, removesProjectFromList: true }),
          ],
        )
        await transaction.execute(
          `INSERT INTO release_resources
            (public_id, release_id, project_id, kind, data_id, group_name, operation,
             publish_order, required_resource, payload_document_id, base_observation_id,
             target_sha256, allocated_project_version, status)
           VALUES (?, ?, ?, 'project_list', ?, ?, 'upsert', 10, TRUE, ?, ?, ?, NULL, 'pending')`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            input.project.id,
            input.dataId,
            input.group,
            targetDocument.internalId,
            observation.insertId.toString(),
            targetDocument.sha256,
          ],
        )
        await transaction.execute(
          `UPDATE projects
           SET status = 'decommissioning', lock_version = lock_version + 1,
               updated_at = CURRENT_TIMESTAMP(6)
           WHERE id = ?`,
          [input.project.id],
        )
        await this.#audit.append(transaction, {
          environmentInternalId: input.project.environment_id,
          actorInternalId: input.actor.internalId,
          eventType: 'project.decommission_requested',
          targetType: 'project',
          targetPublicId: bufferToPublicId(input.project.public_id),
          requestId: input.requestId,
          result: 'success',
          summary: {
            releaseId: releasePublicId,
            domain: input.project.name,
            reason: input.reason,
            expectedLockVersion: input.expectedLockVersion,
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const release = await this.findByPublicId(releasePublicId)
    if (!release) throw new Error('Decommission Release could not be reloaded after creation')
    return { release, replay }
  }

  async markValidationFailed(releaseId: string, errors: readonly unknown[]): Promise<void> {
    await this.#pool.execute(
      `UPDATE releases
       SET status = 'validation_failed', validation_errors_json = ?, lock_version = lock_version + 1
       WHERE public_id = ? AND status = 'validating'`,
      [JSON.stringify(errors), publicIdToBuffer(releaseId)],
    )
  }

  async markAbandoned(releaseId: string, errorCode: string): Promise<void> {
    await this.#pool.execute(
      `UPDATE releases
       SET status = 'abandoned', validation_errors_json = JSON_ARRAY(JSON_OBJECT('code', ?)),
           lock_version = lock_version + 1
       WHERE public_id = ? AND status IN ('creating', 'validating')`,
      [errorCode, publicIdToBuffer(releaseId)],
    )
  }

  async completePreparation(
    releaseId: string,
    project: ProjectIdentityRow,
    source: StoredConfigurationVersion,
    allocatedWireVersion: number,
    resources: readonly PreparedReleaseResource[],
    diffSummary: Readonly<Record<string, unknown>>,
  ): Promise<void> {
    const prepared = resources.map((resource) => ({
      ...resource,
      targetEncrypted: this.#documents.encrypt(Buffer.from(resource.targetContent, 'utf8')),
      baseEncrypted:
        resource.base.exists && resource.base.content !== null
          ? this.#documents.encrypt(Buffer.from(resource.base.content, 'utf8'))
          : null,
    }))

    await withTransaction(this.#pool, async (transaction) => {
      const [releaseRows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
        `SELECT id FROM releases WHERE public_id = ? AND status = 'validating' FOR UPDATE`,
        [publicIdToBuffer(releaseId)],
      )
      const release = releaseRows[0]
      if (!release) throw conflict('RELEASE_NOT_PREPARING', 'Release is no longer being prepared')

      const resourceIds = new Map<PreparedReleaseResource['kind'], string>()
      for (const resource of prepared) {
        let baseDocumentId: string | null = null
        if (resource.baseEncrypted) {
          const baseDocument = await this.#documents.insert(transaction, {
            environmentInternalId: project.environment_id,
            purpose: 'nacos_observation',
            contentType: 'text/plain',
            schemaVersion: null,
            encrypted: resource.baseEncrypted,
          })
          baseDocumentId = baseDocument.internalId
        }
        const observationPublicId = createPublicId()
        const [observationResult] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO nacos_resource_observations
            (public_id, environment_id, data_id, group_name, resource_exists,
             payload_document_id, nacos_md5, sha256, source, client_result, fetched_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'release_preflight', 'success', CURRENT_TIMESTAMP(6))`,
          [
            publicIdToBuffer(observationPublicId),
            project.environment_id,
            resource.dataId,
            resource.group,
            resource.base.exists,
            baseDocumentId,
            resource.base.md5 ? Buffer.from(resource.base.md5, 'hex') : null,
            resource.base.sha256 ? Buffer.from(resource.base.sha256, 'hex') : null,
          ],
        )
        const targetDocument = await this.#documents.insert(transaction, {
          environmentInternalId: project.environment_id,
          purpose: 'release_payload',
          contentType: resource.contentType === 'json' ? 'application/json' : 'text/plain',
          schemaVersion: resource.kind === 'project_route' ? 1 : null,
          encrypted: resource.targetEncrypted,
        })
        const resourcePublicId = createPublicId()
        const [resourceResult] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO release_resources
            (public_id, release_id, project_id, kind, data_id, group_name, operation,
             publish_order, required_resource, payload_document_id, base_observation_id,
             target_sha256, allocated_project_version, status)
           VALUES (?, ?, ?, ?, ?, ?, 'upsert', ?, TRUE, ?, ?, ?, ?, 'pending')`,
          [
            publicIdToBuffer(resourcePublicId),
            release.id,
            project.id,
            resource.kind,
            resource.dataId,
            resource.group,
            resource.publishOrder,
            targetDocument.internalId,
            observationResult.insertId.toString(),
            targetDocument.sha256,
            resource.kind === 'project_route' ? allocatedWireVersion : null,
          ],
        )
        resourceIds.set(resource.kind, resourceResult.insertId.toString())
      }
      for (const resource of prepared) {
        if (!resource.dependsOnKind) continue
        const resourceId = resourceIds.get(resource.kind)
        const dependsOnId = resourceIds.get(resource.dependsOnKind)
        if (!resourceId || !dependsOnId) throw new Error('Release resource dependency is invalid')
        await transaction.execute(
          `INSERT INTO release_resource_dependencies (resource_id, depends_on_resource_id)
           VALUES (?, ?)`,
          [resourceId, dependsOnId],
        )
      }
      await transaction.execute(
        `UPDATE release_items
         SET diff_summary_json = ?
         WHERE release_id = ? AND kind = 'project_route' AND draft_revision_id = ?`,
        [JSON.stringify(diffSummary), release.id, source.internalId],
      )
      await transaction.execute(
        `UPDATE releases
         SET status = 'ready', ready_at = CURRENT_TIMESTAMP(6), lock_version = lock_version + 1
         WHERE id = ?`,
        [release.id],
      )
    })
  }

  async findByPublicId(releasePublicId: string): Promise<ProjectReleaseView | null> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease} WHERE rel.public_id = ? LIMIT 1`,
      [publicIdToBuffer(releasePublicId)],
    )
    return rows[0] ? this.toView(rows[0]) : null
  }

  async listByProject(projectInternalId: string): Promise<readonly ProjectReleaseView[]> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease} WHERE p.id = ? ORDER BY rel.id DESC LIMIT 100`,
      [projectInternalId],
    )
    return Promise.all(rows.map((row) => this.toView(row)))
  }

  async queuePublication(
    actor: Actor,
    releasePublicId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueuePublicationResult> {
    let jobPublicId = createPublicId()
    let jobState = 'queued'
    await withTransaction(this.#pool, async (transaction) => {
      const [releaseRows] = await transaction.execute<
        (RowDataPacket & { id: string; environment_id: string; status: ReleaseStatus })[]
      >('SELECT id, environment_id, status FROM releases WHERE public_id = ? FOR UPDATE', [
        publicIdToBuffer(releasePublicId),
      ])
      const release = releaseRows[0]
      if (!release) throw notFound('Release')
      const [existingRows] = await transaction.execute<
        (RowDataPacket & { public_id: Buffer; state: string })[]
      >('SELECT public_id, state FROM publication_jobs WHERE release_id = ?', [release.id])
      const existing = existingRows[0]
      if (existing) {
        jobPublicId = bufferToPublicId(existing.public_id)
        jobState = existing.state
        return
      }
      if (release.status !== 'ready') {
        throw conflict('RELEASE_NOT_READY', 'Only a ready Release can be published')
      }
      await transaction.execute(
        `INSERT INTO publication_jobs
          (public_id, release_id, state, requested_by, requested_at, next_run_at)
         VALUES (?, ?, 'queued', ?, CURRENT_TIMESTAMP(6), CURRENT_TIMESTAMP(6))`,
        [publicIdToBuffer(jobPublicId), release.id, actor.internalId],
      )
      await transaction.execute(
        `UPDATE releases SET status = 'queued', lock_version = lock_version + 1 WHERE id = ?`,
        [release.id],
      )
      await this.#audit.append(transaction, {
        environmentInternalId: release.environment_id,
        actorInternalId: actor.internalId,
        eventType: 'publication.queued',
        targetType: 'release',
        targetPublicId: releasePublicId,
        requestId,
        result: 'success',
        summary: { jobId: jobPublicId, idempotencyKey },
      })
    })
    const release = await this.findByPublicId(releasePublicId)
    if (!release) throw notFound('Release')
    return { jobId: jobPublicId, state: jobState, release }
  }

  private async toView(row: ReleaseRow): Promise<ProjectReleaseView> {
    const [resourceRows] = await this.#pool.execute<ResourceRow[]>(
      `SELECT public_id, kind, data_id, group_name, operation, status,
              target_sha256, verified_sha256, verified_at
       FROM release_resources
       WHERE release_id = ?
       ORDER BY publish_order, id`,
      [row.internal_id],
    )
    return {
      id: bufferToPublicId(row.public_id),
      sequence: row.sequence_no,
      projectId: bufferToPublicId(row.project_public_id),
      kind: row.kind,
      title: row.title,
      description: row.description,
      status: row.status,
      sourceConfigurationVersion:
        row.source_version_public_id && row.source_version_no !== null
          ? {
              id: bufferToPublicId(row.source_version_public_id),
              number: row.source_version_no,
              relationAtCreation: row.source_relation ?? 'unknown',
            }
          : null,
      currentConfigurationVersionAtCreation:
        row.current_version_public_id && row.current_version_no !== null
          ? {
              id: bufferToPublicId(row.current_version_public_id),
              number: row.current_version_no,
            }
          : null,
      allocatedWireVersion: row.allocated_project_version,
      sourceModelSha256: row.source_model_sha256.toString('hex'),
      wireSha256: row.wire_sha256?.toString('hex') ?? null,
      nativeValidator:
        row.native_validator_contract !== null && row.native_validator_revision !== null
          ? {
              contractVersion: row.native_validator_contract,
              revision: row.native_validator_revision,
            }
          : null,
      compilerRevision: row.compiler_revision,
      validationErrors: parseJsonArray(row.validation_errors_json),
      resources: resourceRows.map(resourceView),
      publication: {
        jobId: row.publication_job_public_id
          ? bufferToPublicId(row.publication_job_public_id)
          : null,
        state: row.publication_job_state,
      },
      activationStatus: 'unknown',
      createdAt: mysqlDateTimeToRfc3339(row.created_at),
      publishedAt: row.published_at ? mysqlDateTimeToRfc3339(row.published_at) : null,
    }
  }
}
