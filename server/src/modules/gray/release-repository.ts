import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import type { NacosResourceValue } from '../../integrations/nacos/model.js'
import { conflict, notFound } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { sha256 } from '../../shared/json.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { ActivationSummary } from '../activation/model.js'
import { unknownActivationSummary } from '../activation/model.js'
import type { ActivationReadRepository } from '../activation/read-repository.js'
import type { Actor } from '../auth/model.js'
import {
  countEffectiveGrayRules,
  GRAY_COMPILER_REVISION,
  GRAY_MATCH_DATA_ID,
  GRAY_MATCH_GROUP,
  isGrayRuleEntry,
  parseGrayRulesWire,
  type GrayRuleEntry,
} from './model.js'
import type { GrayReleaseView, QueueGrayPublicationResult } from './release-model.js'

export interface PreparedGrayReleasePlan {
  wirePayload: string
  ruleCount: number
  entries: readonly GrayRuleEntry[]
}

export interface GrayReleaseCreateMetadata {
  idempotencyKey: string
  requestSha256: Buffer
  compilerRevision: string
  validatorContractVersion: number
  validatorRevision: string
  description?: string
  rollbackOfSequence?: string
}

export interface GrayRollbackSource {
  sequence: string
  compilerRevision: string
  plan: PreparedGrayReleasePlan
}

interface ReleaseRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  sequence_no: string
  status: GrayReleaseView['status']
  description: string
  diff_summary_json: unknown
  wire_sha256: Buffer
  resource_public_id: Buffer
  resource_status: GrayReleaseView['resource']['status']
  verified_sha256: Buffer | null
  verified_at: string | null
  job_public_id: Buffer | null
  job_state: string | null
  created_at: string
  published_at: string | null
}

interface ExistingReleaseRow extends RowDataPacket {
  public_id: Buffer
  request_sha256: Buffer
}

interface RollbackSourceRow extends RowDataPacket {
  sequence_no: string
  compiler_revision: string
  payload_document_id: string
  target_sha256: Buffer
}

const selectRelease = `
  SELECT
    rel.id AS internal_id, rel.public_id, rel.sequence_no, rel.status, rel.description,
    item.diff_summary_json,
    resource.target_sha256 AS wire_sha256,
    resource.public_id AS resource_public_id, resource.status AS resource_status,
    resource.verified_sha256, resource.verified_at,
    job.public_id AS job_public_id, job.state AS job_state,
    rel.created_at, rel.published_at
  FROM releases rel
  INNER JOIN release_items item
    ON item.release_id = rel.id AND item.kind = 'gray_rules'
  INNER JOIN release_resources resource
    ON resource.release_id = rel.id AND resource.kind = 'gray_rules'
  LEFT JOIN publication_jobs job ON job.release_id = rel.id
`

function parseDiffSummary(raw: unknown): { ruleCount: number; entries: readonly GrayRuleEntry[] } {
  const value =
    typeof raw === 'string'
      ? (() => {
          try {
            return JSON.parse(raw) as unknown
          } catch {
            return {}
          }
        })()
      : raw
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    return { ruleCount: 0, entries: [] }
  }
  const record = value as Record<string, unknown>
  const ruleCount =
    typeof record.ruleCount === 'number' &&
    Number.isInteger(record.ruleCount) &&
    record.ruleCount >= 0
      ? record.ruleCount
      : 0
  const entries = Array.isArray(record.entries)
    ? record.entries.filter((entry): entry is GrayRuleEntry => isGrayRuleEntry(entry))
    : []
  return { ruleCount, entries }
}

export class GrayRuleReleaseRepository {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #audit: AuditRepository
  readonly #activation: ActivationReadRepository | null

  constructor(
    pool: DatabasePool,
    documents: DocumentRepository,
    audit = new AuditRepository(),
    activation: ActivationReadRepository | null = null,
  ) {
    this.#pool = pool
    this.#documents = documents
    this.#audit = audit
    this.#activation = activation
  }

  async create(
    actor: Actor,
    environmentInternalId: string,
    plan: PreparedGrayReleasePlan,
    base: NacosResourceValue,
    metadata: GrayReleaseCreateMetadata,
    requestId: string,
  ): Promise<GrayReleaseView> {
    let releasePublicId = createPublicId()
    const payloadBuffer = Buffer.from(plan.wirePayload, 'utf8')
    const payloadDigest = sha256(payloadBuffer)
    const encryptedPayload = this.#documents.encrypt(payloadBuffer)
    const encryptedBase =
      base.exists && base.content !== null
        ? this.#documents.encrypt(Buffer.from(base.content, 'utf8'))
        : null

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [existingRows] = await transaction.execute<ExistingReleaseRow[]>(
          `SELECT public_id, request_sha256
           FROM releases
           WHERE environment_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [environmentInternalId, actor.internalId, metadata.idempotencyKey],
        )
        const existing = existingRows[0]
        if (existing) {
          if (!existing.request_sha256.equals(metadata.requestSha256)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with a different gray release request',
            )
          }
          releasePublicId = bufferToPublicId(existing.public_id)
          return
        }

        const [environmentRows] = await transaction.execute<
          (RowDataPacket & { last_release_sequence: string })[]
        >('SELECT last_release_sequence FROM environments WHERE id = ? FOR UPDATE', [
          environmentInternalId,
        ])
        const environment = environmentRows[0]
        if (!environment) throw notFound('Deployment workspace')
        const sequence = BigInt(environment.last_release_sequence) + 1n

        const payloadDocument = await this.#documents.insert(transaction, {
          environmentInternalId,
          purpose: 'release_payload',
          contentType: 'application/json',
          schemaVersion: 1,
          encrypted: encryptedPayload,
        })
        let baseDocumentId: string | null = null
        if (encryptedBase) {
          const baseDocument = await this.#documents.insert(transaction, {
            environmentInternalId,
            purpose: 'nacos_observation',
            contentType: 'application/json',
            schemaVersion: 1,
            encrypted: encryptedBase,
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
            environmentInternalId,
            GRAY_MATCH_DATA_ID,
            GRAY_MATCH_GROUP,
            base.exists,
            baseDocumentId,
            base.md5 ? Buffer.from(base.md5, 'hex') : null,
            base.sha256 ? Buffer.from(base.sha256, 'hex') : null,
          ],
        )

        await transaction.execute(
          'UPDATE environments SET last_release_sequence = ? WHERE id = ?',
          [sequence.toString(), environmentInternalId],
        )
        const [release] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO releases
            (public_id, environment_id, sequence_no, kind, status, title, description,
             compiler_revision, idempotency_key, request_sha256, validation_errors_json,
             native_validator_contract, native_validator_revision, created_by, ready_at)
           VALUES (?, ?, ?, 'gray_rules', 'ready', ?, ?, ?, ?, ?,
                   JSON_ARRAY(), ?, ?, ?, CURRENT_TIMESTAMP(6))`,
          [
            publicIdToBuffer(releasePublicId),
            environmentInternalId,
            sequence.toString(),
            `Gray rules #${sequence.toString()}`,
            metadata.description ?? '',
            metadata.compilerRevision,
            metadata.idempotencyKey,
            metadata.requestSha256,
            metadata.validatorContractVersion,
            metadata.validatorRevision,
            actor.internalId,
          ],
        )
        const releaseInternalId = release.insertId.toString()
        await transaction.execute(
          `INSERT INTO release_items
            (public_id, release_id, project_id, kind, draft_revision_id, source_relation,
             model_document_id, allocated_project_version, change_kind, diff_summary_json)
           VALUES (?, ?, NULL, 'gray_rules', NULL, NULL, ?, NULL, 'update', ?)`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            payloadDocument.internalId,
            JSON.stringify({
              ruleCount: plan.ruleCount,
              entries: plan.entries,
              ...(metadata.rollbackOfSequence !== undefined
                ? { rollbackOfSequence: metadata.rollbackOfSequence }
                : {}),
            }),
          ],
        )
        await transaction.execute(
          `INSERT INTO release_resources
            (public_id, release_id, project_id, kind, data_id, group_name, operation,
             publish_order, required_resource, payload_document_id, base_observation_id,
             target_sha256, allocated_project_version, status)
           VALUES (?, ?, NULL, 'gray_rules', ?, ?, 'upsert', 10, TRUE, ?, ?, ?, NULL, 'pending')`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            GRAY_MATCH_DATA_ID,
            GRAY_MATCH_GROUP,
            payloadDocument.internalId,
            observation.insertId.toString(),
            payloadDigest,
          ],
        )
        await this.#audit.append(transaction, {
          environmentInternalId,
          actorInternalId: actor.internalId,
          eventType: metadata.rollbackOfSequence
            ? 'gray.release.rollback_created'
            : 'gray.release.created',
          targetType: 'release',
          targetPublicId: releasePublicId,
          requestId,
          result: 'success',
          summary: {
            sequence: sequence.toString(),
            ruleCount: plan.ruleCount,
            entries: plan.entries,
            wireSha256: payloadDigest.toString('hex'),
            ...(metadata.rollbackOfSequence !== undefined
              ? { rollbackOfSequence: metadata.rollbackOfSequence }
              : {}),
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const result = await this.find(releasePublicId, environmentInternalId)
    if (!result) throw new Error('Gray release could not be reloaded')
    return result
  }

  async loadRollbackSource(
    environmentInternalId: string,
    releasePublicId: string,
  ): Promise<GrayRollbackSource | null> {
    const [rows] = await this.#pool.execute<RollbackSourceRow[]>(
      `SELECT rel.sequence_no, rel.compiler_revision,
              resource.payload_document_id, resource.target_sha256
       FROM releases rel
       INNER JOIN release_resources resource
         ON resource.release_id = rel.id AND resource.kind = 'gray_rules'
       WHERE rel.public_id = ? AND rel.environment_id = ? AND rel.kind = 'gray_rules'
       LIMIT 1`,
      [publicIdToBuffer(releasePublicId), environmentInternalId],
    )
    const row = rows[0]
    if (!row) return null
    const plaintext = await this.#documents.decryptByInternalId(this.#pool, row.payload_document_id)
    if (!plaintext) {
      throw new Error('Historical gray release payload could not be decrypted')
    }
    if (!sha256(plaintext).equals(row.target_sha256)) {
      throw new Error('Historical gray release payload digest mismatch')
    }
    const wirePayload = plaintext.toString('utf8')
    const parsed = parseGrayRulesWire(wirePayload)
    if (!parsed.document) {
      throw new Error(
        `Historical gray release payload could not be parsed: ${parsed.parseError ?? 'unknown'}`,
      )
    }
    return {
      sequence: row.sequence_no,
      compilerRevision: row.compiler_revision || GRAY_COMPILER_REVISION,
      plan: {
        wirePayload,
        ruleCount: countEffectiveGrayRules(parsed.document),
        entries: parsed.document.rules.map((rule) => rule.entry),
      },
    }
  }

  async list(environmentInternalId: string): Promise<readonly GrayReleaseView[]> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease}
       WHERE rel.environment_id = ? AND rel.kind = 'gray_rules'
       ORDER BY rel.id DESC LIMIT 100`,
      [environmentInternalId],
    )
    const summaries = this.#activation
      ? await this.#activation.summariesForReleases(rows.map((row) => row.internal_id))
      : new Map<string, ActivationSummary>()
    return rows.map((row) =>
      this.#toView(row, summaries.get(row.internal_id) ?? unknownActivationSummary()),
    )
  }

  async find(publicId: string, environmentInternalId?: string): Promise<GrayReleaseView | null> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease}
       WHERE rel.public_id = ? AND rel.kind = 'gray_rules'
         ${environmentInternalId ? 'AND rel.environment_id = ?' : ''}
       LIMIT 1`,
      environmentInternalId
        ? [publicIdToBuffer(publicId), environmentInternalId]
        : [publicIdToBuffer(publicId)],
    )
    const row = rows[0]
    if (!row) return null
    const activation = this.#activation
      ? await this.#activation.summaryForRelease(row.internal_id)
      : unknownActivationSummary()
    return this.#toView(row, activation)
  }

  async queuePublication(
    actor: Actor,
    releasePublicId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueGrayPublicationResult> {
    let jobPublicId = createPublicId()
    let jobState = 'queued'
    await withTransaction(this.#pool, async (transaction) => {
      const [releaseRows] = await transaction.execute<
        (RowDataPacket & { id: string; environment_id: string; status: string })[]
      >(
        `SELECT id, environment_id, status
         FROM releases WHERE public_id = ? AND kind = 'gray_rules' FOR UPDATE`,
        [publicIdToBuffer(releasePublicId)],
      )
      const release = releaseRows[0]
      if (!release) throw notFound('Gray release')
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
        throw conflict('RELEASE_NOT_READY', 'Only a ready gray release can be published')
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
        summary: { jobId: jobPublicId, idempotencyKey, kind: 'gray_rules' },
      })
    })
    const release = await this.find(releasePublicId)
    if (!release) throw notFound('Gray release')
    return { jobId: jobPublicId, state: jobState, release }
  }

  #toView(
    row: ReleaseRow,
    activation: ActivationSummary = unknownActivationSummary(),
  ): GrayReleaseView {
    const summary = parseDiffSummary(row.diff_summary_json)
    return {
      id: bufferToPublicId(row.public_id),
      sequence: row.sequence_no,
      status: row.status,
      ruleCount: summary.ruleCount,
      entries: summary.entries,
      wireSha256: row.wire_sha256.toString('hex'),
      description: row.description,
      resource: {
        id: bufferToPublicId(row.resource_public_id),
        dataId: GRAY_MATCH_DATA_ID,
        group: GRAY_MATCH_GROUP,
        status: row.resource_status,
        verifiedSha256: row.verified_sha256?.toString('hex') ?? null,
        verifiedAt: row.verified_at ? mysqlDateTimeToRfc3339(row.verified_at) : null,
      },
      publication: {
        jobId: row.job_public_id ? bufferToPublicId(row.job_public_id) : null,
        state: row.job_state,
      },
      activationStatus: activation.status,
      activation,
      createdAt: mysqlDateTimeToRfc3339(row.created_at),
      publishedAt: row.published_at ? mysqlDateTimeToRfc3339(row.published_at) : null,
    }
  }
}
