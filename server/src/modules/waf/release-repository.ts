// WAF release persistence. Mirrors the TLS snapshot release pattern
// (tls/release-repository.ts) for an environment-wide, non-project release
// kind, extended to the edge-function two-resource shape required by
// docs/edge-functions-and-waf-design.md §3.1/§4.3: the script Data ID is
// written first (publish_order 10), the index Data ID last (publish_order 20)
// and carries a dependency edge on the script, so the publication worker can
// never expose an index whose referenced content does not exist yet.

import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import type { NacosResourceValue } from '../../integrations/nacos/model.js'
import { conflict, notFound, unprocessable } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import type { ActivationSummary } from '../activation/model.js'
import { unknownActivationSummary } from '../activation/model.js'
import type { ActivationReadRepository } from '../activation/read-repository.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import {
  EDGE_FUNCTIONS_DATA_ID,
  EDGE_FUNCTIONS_GROUP,
  edgeFunctionScriptDataId,
  type QueueWafPublicationResult,
  type WafReleaseResourceView,
  type WafReleaseView,
} from './release-model.js'

/** One function entry of the wire index (the exact codec shape). */
export interface WafWireIndexFunctionEntry {
  readonly name: string
  readonly sha256: string
  readonly mounts: readonly {
    readonly projects: { readonly include: readonly string[]; readonly exclude: readonly string[] }
    readonly priority: number
  }[]
}

/** Everything the repository needs to persist one compiled WAF release. */
export interface PreparedWafReleasePlan {
  readonly mode: 'mounted' | 'unmount'
  readonly functionName: string
  readonly script: string | null
  readonly scriptSha256: string | null
  readonly functions: readonly WafWireIndexFunctionEntry[]
  readonly ruleCount: number
}

/** A historical release's immutable payloads, rebuilt into a publishable plan. */
export interface WafRollbackSource {
  readonly publicId: string
  readonly sequence: string
  readonly generatorRevision: string
  readonly plan: PreparedWafReleasePlan
}

interface ReleaseRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  sequence_no: string
  status: WafReleaseView['status']
  description: string
  mode: WafReleaseView['mode']
  function_name: string
  script_sha256: Buffer | null
  rule_count: number
  index_version: string
  job_public_id: Buffer | null
  job_state: string | null
  created_at: string
  published_at: string | null
}

interface ResourceRow extends RowDataPacket {
  release_internal_id: string
  public_id: Buffer
  kind: WafReleaseResourceView['kind']
  data_id: string
  group_name: string
  status: WafReleaseResourceView['status']
  verified_sha256: Buffer | null
  verified_at: string | null
}

const selectRelease = `
  SELECT
    rel.id AS internal_id, rel.public_id, rel.sequence_no, rel.status,
    rel.description,
    JSON_UNQUOTE(JSON_EXTRACT(item.diff_summary_json, '$.mode')) AS mode,
    JSON_UNQUOTE(JSON_EXTRACT(item.diff_summary_json, '$.functionName')) AS function_name,
    FROM_HEX(JSON_UNQUOTE(JSON_EXTRACT(item.diff_summary_json, '$.scriptSha256'))) AS script_sha256,
    CAST(JSON_UNQUOTE(JSON_EXTRACT(item.diff_summary_json, '$.ruleCount')) AS UNSIGNED)
      AS rule_count,
    CAST(JSON_UNQUOTE(JSON_EXTRACT(item.diff_summary_json, '$.indexVersion')) AS CHAR)
      AS index_version,
    job.public_id AS job_public_id, job.state AS job_state,
    rel.created_at, rel.published_at
  FROM releases rel
  INNER JOIN release_items item
    ON item.release_id = rel.id AND item.kind = 'edge_functions'
  LEFT JOIN publication_jobs job ON job.release_id = rel.id
`

function publishedIndexVersion(base: NacosResourceValue): bigint {
  if (!base.exists || base.content === null) return 0n
  try {
    const value: unknown = JSON.parse(base.content)
    if (!value || typeof value !== 'object' || Array.isArray(value)) return 0n
    const version = (value as { version?: unknown }).version
    if (
      typeof version !== 'number' ||
      !Number.isSafeInteger(version) ||
      version < 0 ||
      version > 2_147_483_647
    ) {
      return 0n
    }
    return BigInt(version)
  } catch {
    return 0n
  }
}

// Light structural parse of a stored index payload back into wire entries.
// This only rebuilds the rollback plan; the service re-runs the full native
// validator on the rebuilt payloads before anything is persisted.
function parseWireIndexFunctions(text: string): WafWireIndexFunctionEntry[] | null {
  let value: unknown
  try {
    value = JSON.parse(text)
  } catch {
    return null
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null
  const functions = (value as { functions?: unknown }).functions
  if (!Array.isArray(functions)) return null
  const entries: WafWireIndexFunctionEntry[] = []
  for (const entry of functions) {
    if (!entry || typeof entry !== 'object' || Array.isArray(entry)) return null
    const record = entry as Record<string, unknown>
    if (
      typeof record.name !== 'string' ||
      typeof record.sha256 !== 'string' ||
      !/^[0-9a-f]{64}$/u.test(record.sha256) ||
      !Array.isArray(record.mounts)
    ) {
      return null
    }
    const mounts: WafWireIndexFunctionEntry['mounts'][number][] = []
    for (const mount of record.mounts) {
      if (!mount || typeof mount !== 'object' || Array.isArray(mount)) return null
      const mountRecord = mount as Record<string, unknown>
      const projects = mountRecord.projects
      if (!projects || typeof projects !== 'object' || Array.isArray(projects)) return null
      const projectsRecord = projects as Record<string, unknown>
      const include = projectsRecord.include
      const exclude = projectsRecord.exclude ?? []
      if (!Array.isArray(include) || include.some((item) => typeof item !== 'string')) return null
      if (!Array.isArray(exclude) || exclude.some((item) => typeof item !== 'string')) return null
      if (typeof mountRecord.priority !== 'number' || !Number.isSafeInteger(mountRecord.priority)) {
        return null
      }
      mounts.push({
        projects: { include: [...include], exclude: [...exclude] },
        priority: mountRecord.priority,
      })
    }
    entries.push({ name: record.name, sha256: record.sha256, mounts })
  }
  return entries
}

interface RollbackSourceRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  sequence_no: string
  compiler_revision: string
  diff_summary_json: string
  script_document_id: string | null
  script_target_sha256: Buffer | null
  index_document_id: string | null
  index_target_sha256: Buffer | null
}

export class WafReleaseRepository {
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
    plan: PreparedWafReleasePlan,
    bases: { index: NacosResourceValue; script: NacosResourceValue | null },
    input: {
      idempotencyKey: string
      requestSha256: Buffer
      generatorRevision: string
      validatorContractVersion: number
      validatorRevision: string
      description?: string
      rollbackOfSequence?: string | null
    },
    requestId: string,
  ): Promise<WafReleaseView> {
    if (plan.mode === 'mounted' && (plan.script === null || plan.scriptSha256 === null)) {
      throw new Error('Mounted WAF release plan is missing its script')
    }
    if (plan.mode === 'unmount' && bases.script !== null) {
      throw new Error('Unmount WAF release plan must not carry a script base observation')
    }

    let releasePublicId = createPublicId()
    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [existingRows] = await transaction.execute<
          (RowDataPacket & { public_id: Buffer; request_sha256: Buffer })[]
        >(
          `SELECT public_id, request_sha256
           FROM releases
           WHERE environment_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [environmentInternalId, actor.internalId, input.idempotencyKey],
        )
        const existing = existingRows[0]
        if (existing) {
          if (!existing.request_sha256.equals(input.requestSha256)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with a different WAF release request',
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
        const lastReleaseSequence = BigInt(environment.last_release_sequence)
        const baseVersion = publishedIndexVersion(bases.index)
        const sequence =
          (lastReleaseSequence > baseVersion ? lastReleaseSequence : baseVersion) + 1n
        if (sequence > 2_147_483_647n) {
          throw conflict(
            'EDGE_INDEX_VERSION_EXHAUSTED',
            'The edge-function index version range is exhausted',
          )
        }

        const indexJson = canonicalJson({
          version: Number(sequence),
          functions: plan.functions,
        })
        const indexSha256 = sha256(indexJson)
        const scriptDataId = edgeFunctionScriptDataId(plan.functionName)

        const indexDocument = await this.#documents.insert(transaction, {
          environmentInternalId,
          purpose: 'release_payload',
          contentType: 'application/json',
          schemaVersion: 1,
          encrypted: this.#documents.encrypt(Buffer.from(indexJson, 'utf8')),
        })
        let scriptDocumentId: string | null = null
        if (plan.mode === 'mounted' && plan.script !== null) {
          const scriptDocument = await this.#documents.insert(transaction, {
            environmentInternalId,
            purpose: 'release_payload',
            contentType: 'text/plain',
            schemaVersion: 1,
            encrypted: this.#documents.encrypt(Buffer.from(plan.script, 'utf8')),
          })
          scriptDocumentId = scriptDocument.internalId
        }

        const observations: { kind: 'edge_function_index' | 'edge_function_script'; id: string }[] =
          []
        for (const observed of [
          {
            kind: 'edge_function_index' as const,
            dataId: EDGE_FUNCTIONS_DATA_ID,
            base: bases.index,
          },
          ...(bases.script
            ? [
                {
                  kind: 'edge_function_script' as const,
                  dataId: scriptDataId,
                  base: bases.script,
                },
              ]
            : []),
        ]) {
          let baseDocumentId: string | null = null
          if (observed.base.exists && observed.base.content !== null) {
            const baseDocument = await this.#documents.insert(transaction, {
              environmentInternalId,
              purpose: 'nacos_observation',
              contentType:
                observed.kind === 'edge_function_index' ? 'application/json' : 'text/plain',
              schemaVersion: 1,
              encrypted: this.#documents.encrypt(Buffer.from(observed.base.content, 'utf8')),
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
              observed.dataId,
              EDGE_FUNCTIONS_GROUP,
              observed.base.exists,
              baseDocumentId,
              observed.base.md5 ? Buffer.from(observed.base.md5, 'hex') : null,
              observed.base.sha256 ? Buffer.from(observed.base.sha256, 'hex') : null,
            ],
          )
          observations.push({ kind: observed.kind, id: observation.insertId.toString() })
        }
        const indexObservationId = observations.find(
          (observation) => observation.kind === 'edge_function_index',
        )?.id
        const scriptObservationId = observations.find(
          (observation) => observation.kind === 'edge_function_script',
        )?.id
        if (!indexObservationId) throw new Error('Index base observation is missing')

        await transaction.execute(
          'UPDATE environments SET last_release_sequence = ? WHERE id = ?',
          [sequence.toString(), environmentInternalId],
        )
        const [release] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO releases
            (public_id, environment_id, sequence_no, kind, status, title, description,
             compiler_revision, idempotency_key, request_sha256, validation_errors_json,
             native_validator_contract, native_validator_revision, created_by, ready_at)
           VALUES (?, ?, ?, 'edge_functions', 'ready', ?, ?, ?, ?, ?, JSON_ARRAY(), ?, ?, ?,
                   CURRENT_TIMESTAMP(6))`,
          [
            publicIdToBuffer(releasePublicId),
            environmentInternalId,
            sequence.toString(),
            `WAF edge functions #${sequence.toString()}`,
            input.description ?? '',
            input.generatorRevision,
            input.idempotencyKey,
            input.requestSha256,
            input.validatorContractVersion,
            input.validatorRevision,
            actor.internalId,
          ],
        )
        const releaseInternalId = release.insertId.toString()
        await transaction.execute(
          `INSERT INTO release_items
            (public_id, release_id, project_id, kind, draft_revision_id, source_relation,
             model_document_id, allocated_project_version, change_kind, diff_summary_json)
           VALUES (?, ?, NULL, 'edge_functions', NULL, NULL, ?, ?, 'update', ?)`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            indexDocument.internalId,
            Number(sequence),
            JSON.stringify({
              functionName: plan.functionName,
              mode: plan.mode,
              scriptSha256: plan.mode === 'mounted' ? plan.scriptSha256 : null,
              ruleCount: plan.ruleCount,
              indexVersion: sequence.toString(),
            }),
          ],
        )

        let scriptResourceId: string | null = null
        if (
          plan.mode === 'mounted' &&
          scriptDocumentId !== null &&
          scriptObservationId !== undefined &&
          plan.scriptSha256 !== null
        ) {
          const [scriptResource] = await transaction.execute<ResultSetHeader>(
            `INSERT INTO release_resources
              (public_id, release_id, project_id, kind, data_id, group_name, operation,
               publish_order, required_resource, payload_document_id, base_observation_id,
               target_sha256, allocated_project_version, status)
             VALUES (?, ?, NULL, 'edge_function_script', ?, ?, 'upsert', 10, TRUE, ?, ?, ?, ?, 'pending')`,
            [
              publicIdToBuffer(createPublicId()),
              releaseInternalId,
              scriptDataId,
              EDGE_FUNCTIONS_GROUP,
              scriptDocumentId,
              scriptObservationId,
              Buffer.from(plan.scriptSha256, 'hex'),
              Number(sequence),
            ],
          )
          scriptResourceId = scriptResource.insertId.toString()
        }
        const [indexResource] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO release_resources
            (public_id, release_id, project_id, kind, data_id, group_name, operation,
             publish_order, required_resource, payload_document_id, base_observation_id,
             target_sha256, allocated_project_version, status)
           VALUES (?, ?, NULL, 'edge_function_index', ?, ?, 'upsert', 20, TRUE, ?, ?, ?, ?, 'pending')`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            EDGE_FUNCTIONS_DATA_ID,
            EDGE_FUNCTIONS_GROUP,
            indexDocument.internalId,
            indexObservationId,
            indexSha256,
            Number(sequence),
          ],
        )
        if (scriptResourceId) {
          await transaction.execute(
            `INSERT INTO release_resource_dependencies (resource_id, depends_on_resource_id)
             VALUES (?, ?)`,
            [indexResource.insertId.toString(), scriptResourceId],
          )
        }

        await this.#audit.append(transaction, {
          environmentInternalId,
          actorInternalId: actor.internalId,
          eventType: 'waf.release_created',
          targetType: 'release',
          targetPublicId: releasePublicId,
          requestId,
          result: 'success',
          summary: {
            sequence: sequence.toString(),
            mode: plan.mode,
            functionName: plan.functionName,
            scriptSha256: plan.mode === 'mounted' ? plan.scriptSha256 : null,
            ruleCount: plan.ruleCount,
            ...(input.rollbackOfSequence ? { rollbackOf: input.rollbackOfSequence } : {}),
          },
        })
      },
      { retryOnDeadlock: true },
    )
    const result = await this.find(releasePublicId)
    if (!result) throw new Error('WAF release could not be reloaded')
    return result
  }

  // Rollback rebuilds a publishable plan from a historical release's immutable
  // payload documents: content is verified against the digests recorded when
  // the release was created, and the service re-validates it through today's
  // native validator before persisting. The new release allocates a fresh
  // index version; release history is never rewritten. Any persisted release
  // of this environment is an eligible source.
  async loadRollbackSource(
    environmentInternalId: string,
    releasePublicId: string,
  ): Promise<WafRollbackSource | null> {
    const [rows] = await this.#pool.execute<RollbackSourceRow[]>(
      `SELECT rel.id AS internal_id, rel.public_id, rel.sequence_no,
              rel.compiler_revision, item.diff_summary_json,
              script_res.payload_document_id AS script_document_id,
              script_res.target_sha256 AS script_target_sha256,
              index_res.payload_document_id AS index_document_id,
              index_res.target_sha256 AS index_target_sha256
       FROM releases rel
       INNER JOIN release_items item
         ON item.release_id = rel.id AND item.kind = 'edge_functions'
       LEFT JOIN release_resources script_res
         ON script_res.release_id = rel.id AND script_res.kind = 'edge_function_script'
       LEFT JOIN release_resources index_res
         ON index_res.release_id = rel.id AND index_res.kind = 'edge_function_index'
       WHERE rel.public_id = ? AND rel.environment_id = ? AND rel.kind = 'edge_functions'
       LIMIT 1`,
      [publicIdToBuffer(releasePublicId), environmentInternalId],
    )
    const row = rows[0]
    if (!row) return null

    const invalid = (detail: string) =>
      unprocessable(
        'WAF_ROLLBACK_PAYLOAD_INVALID',
        `The historical WAF release payload is unusable: ${detail}`,
      )

    let summary: {
      mode?: unknown
      functionName?: unknown
      scriptSha256?: unknown
      ruleCount?: unknown
    }
    try {
      summary = JSON.parse(row.diff_summary_json) as typeof summary
    } catch {
      throw invalid('the diff summary is not valid JSON')
    }
    if (summary.mode !== 'mounted' && summary.mode !== 'unmount') {
      throw invalid('the diff summary mode is unknown')
    }
    if (typeof summary.functionName !== 'string' || summary.functionName.length === 0) {
      throw invalid('the diff summary function name is missing')
    }
    if (
      typeof summary.ruleCount !== 'number' ||
      !Number.isSafeInteger(summary.ruleCount) ||
      summary.ruleCount < 0
    ) {
      throw invalid('the diff summary rule count is invalid')
    }

    if (!row.index_document_id) throw invalid('the index payload document is missing')
    const indexText =
      (await this.#documents.decryptByInternalId(this.#pool, row.index_document_id))?.toString(
        'utf8',
      ) ?? null
    if (indexText === null) throw invalid('the index payload document is unreadable')
    if (
      row.index_target_sha256 &&
      sha256(indexText).toString('hex') !== row.index_target_sha256.toString('hex')
    ) {
      throw invalid('the index payload digest does not match the release record')
    }
    const functions = parseWireIndexFunctions(indexText)
    if (!functions) throw invalid('the index payload is not a function index')

    if (summary.mode === 'unmount') {
      if (functions.length !== 0) throw invalid('the unmount release carries mounted functions')
      return {
        publicId: bufferToPublicId(row.public_id),
        sequence: row.sequence_no,
        generatorRevision: row.compiler_revision,
        plan: {
          mode: 'unmount',
          functionName: summary.functionName,
          script: null,
          scriptSha256: null,
          functions,
          ruleCount: summary.ruleCount,
        },
      }
    }

    if (!row.script_document_id) throw invalid('the script payload document is missing')
    const scriptText =
      (await this.#documents.decryptByInternalId(this.#pool, row.script_document_id))?.toString(
        'utf8',
      ) ?? null
    if (scriptText === null) throw invalid('the script payload document is unreadable')
    const scriptSha256 = sha256(scriptText).toString('hex')
    if (row.script_target_sha256 && scriptSha256 !== row.script_target_sha256.toString('hex')) {
      throw invalid('the script payload digest does not match the release record')
    }
    // The stored index must reference exactly these script bytes under the
    // release's function name (single-function v1 generator output).
    const indexed = functions.filter((entry) => entry.sha256 === scriptSha256)
    if (indexed.length !== 1 || indexed[0]!.name !== summary.functionName) {
      throw invalid('the stored index does not reference the release script')
    }
    return {
      publicId: bufferToPublicId(row.public_id),
      sequence: row.sequence_no,
      generatorRevision: row.compiler_revision,
      plan: {
        mode: 'mounted',
        functionName: summary.functionName,
        script: scriptText,
        scriptSha256,
        functions,
        ruleCount: summary.ruleCount,
      },
    }
  }

  async list(environmentInternalId: string): Promise<readonly WafReleaseView[]> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease}
       WHERE rel.environment_id = ? AND rel.kind = 'edge_functions'
       ORDER BY rel.id DESC LIMIT 100`,
      [environmentInternalId],
    )
    const summaries = this.#activation
      ? await this.#activation.summariesForReleases(rows.map((row) => row.internal_id))
      : new Map<string, ActivationSummary>()
    const resources = await this.loadResources(rows.map((row) => row.internal_id))
    return rows.map((row) =>
      this.toView(row, resources.get(row.internal_id) ?? [], summaries.get(row.internal_id)),
    )
  }

  async find(publicId: string, environmentInternalId?: string): Promise<WafReleaseView | null> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease}
       WHERE rel.public_id = ? AND rel.kind = 'edge_functions'
         ${environmentInternalId ? 'AND rel.environment_id = ?' : ''}
       LIMIT 1`,
      environmentInternalId
        ? [publicIdToBuffer(publicId), environmentInternalId]
        : [publicIdToBuffer(publicId)],
    )
    const row = rows[0]
    if (!row) return null
    const resources = await this.loadResources([row.internal_id])
    const activation = this.#activation
      ? await this.#activation.summaryForRelease(row.internal_id)
      : unknownActivationSummary()
    return this.toView(row, resources.get(row.internal_id) ?? [], activation)
  }

  async queuePublication(
    actor: Actor,
    releasePublicId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueWafPublicationResult> {
    let jobPublicId = createPublicId()
    let jobState = 'queued'
    await withTransaction(this.#pool, async (transaction) => {
      const [releaseRows] = await transaction.execute<
        (RowDataPacket & { id: string; environment_id: string; status: string })[]
      >(
        `SELECT id, environment_id, status
         FROM releases WHERE public_id = ? AND kind = 'edge_functions' FOR UPDATE`,
        [publicIdToBuffer(releasePublicId)],
      )
      const release = releaseRows[0]
      if (!release) throw notFound('WAF release')
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
        throw conflict('RELEASE_NOT_READY', 'Only a ready WAF release can be published')
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
        summary: { jobId: jobPublicId, idempotencyKey, kind: 'edge_functions' },
      })
    })
    const release = await this.find(releasePublicId)
    if (!release) throw notFound('WAF release')
    return { jobId: jobPublicId, state: jobState, release }
  }

  async loadResources(
    releaseInternalIds: readonly string[],
  ): Promise<Map<string, WafReleaseResourceView[]>> {
    const result = new Map<string, WafReleaseResourceView[]>()
    if (releaseInternalIds.length === 0) return result
    const placeholders = releaseInternalIds.map(() => '?').join(', ')
    const [rows] = await this.#pool.execute<ResourceRow[]>(
      `SELECT release_id AS release_internal_id, public_id, kind, data_id, group_name,
              status, verified_sha256, verified_at
       FROM release_resources
       WHERE release_id IN (${placeholders})
         AND kind IN ('edge_function_script', 'edge_function_index')
       ORDER BY publish_order, id`,
      [...releaseInternalIds],
    )
    for (const row of rows) {
      const key = row.release_internal_id
      const entry: WafReleaseResourceView = {
        id: bufferToPublicId(row.public_id),
        kind: row.kind,
        dataId: row.data_id,
        group: row.group_name,
        status: row.status,
        verifiedSha256: row.verified_sha256?.toString('hex') ?? null,
        verifiedAt: row.verified_at ? mysqlDateTimeToRfc3339(row.verified_at) : null,
      }
      const bucket = result.get(key)
      if (bucket) {
        bucket.push(entry)
      } else {
        result.set(key, [entry])
      }
    }
    return result
  }

  private toView(
    row: ReleaseRow,
    resources: readonly WafReleaseResourceView[],
    activation: ActivationSummary = unknownActivationSummary(),
  ): WafReleaseView {
    return {
      id: bufferToPublicId(row.public_id),
      sequence: row.sequence_no,
      status: row.status,
      functionName: row.function_name,
      mode: row.mode === 'unmount' ? 'unmount' : 'mounted',
      scriptSha256: row.script_sha256?.toString('hex') ?? null,
      ruleCount: Number(row.rule_count),
      indexVersion: row.index_version,
      description: row.description ?? '',
      resources,
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
