import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import { conflict, notFound } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type { ProjectRoutesModel } from '../drafts/model.js'
import { normalizeStoredProjectRoutesModel } from '../drafts/model.js'
import { ROUTE_COMPILER_REVISION } from '../drafts/compiler.js'
import type { ProjectRoutesValidationView } from '../drafts/validation.js'
import type { ProjectIdentityRow } from '../projects/repository.js'
import type {
  ConfigurationVersionDetail,
  ConfigurationVersionSummary,
  SavedConfigurationVersion,
} from './model.js'

interface VersionRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  draft_internal_id: string
  draft_public_id: Buffer
  project_public_id: Buffer
  environment_internal_id: string
  model_document_id: string
  plaintext_sha256: Buffer
  revision_no: number
  current_revision_no: number
  draft_lock_version: string
  parent_public_id: Buffer | null
  restored_from_public_id: Buffer | null
  validation_state: ConfigurationVersionSummary['validationState']
  route_count: number
  idempotency_key: string | null
  change_summary: string
  publication_status: ConfigurationVersionSummary['publicationStatus'] | null
  creator_public_id: Buffer
  creator_display_name: string
  created_at: string
}

interface LockedDraftRow extends RowDataPacket {
  id: string
  environment_id: string
  current_revision_no: number
  lock_version: string
}

interface CurrentVersionRow extends RowDataPacket {
  id: string
  public_id: Buffer
  plaintext_sha256: Buffer
}

export interface StoredConfigurationVersion {
  internalId: string
  draftInternalId: string
  environmentInternalId: string
  projectId: string
  projectInternalId: string
  id: string
  number: number
  modelDocumentInternalId: string
  modelSha256: string
  model: ProjectRoutesModel
}

interface SaveVersionRepositoryInput {
  actor: Actor
  project: ProjectIdentityRow
  expectedLockVersion: string
  baseVersionId: string | null
  changeSummary: string
  forceSameContent: boolean
  idempotencyKey: string
  model: ProjectRoutesModel
  restoredFromVersionId?: string | null
  requestId: string
}

const selectVersions = `
  SELECT
    r.id AS internal_id, r.public_id,
    d.id AS draft_internal_id, d.public_id AS draft_public_id,
    p.public_id AS project_public_id, d.environment_id AS environment_internal_id,
    r.model_document_id, cd.plaintext_sha256,
    r.revision_no, d.current_revision_no, d.lock_version AS draft_lock_version,
    parent.public_id AS parent_public_id,
    restored.public_id AS restored_from_public_id,
    r.validation_state, r.route_count, r.idempotency_key, r.change_summary,
    (
      SELECT rel.status
      FROM release_items ri
      INNER JOIN releases rel ON rel.id = ri.release_id
      WHERE ri.draft_revision_id = r.id
      ORDER BY rel.id DESC
      LIMIT 1
    ) AS publication_status,
    u.public_id AS creator_public_id, u.display_name AS creator_display_name,
    r.created_at
  FROM draft_revisions r
  INNER JOIN drafts d ON d.id = r.draft_id AND d.archived_at IS NULL
  INNER JOIN projects p ON p.id = d.project_id AND p.archived_at IS NULL
  INNER JOIN config_documents cd ON cd.id = r.model_document_id
  INNER JOIN users u ON u.id = r.created_by
  LEFT JOIN draft_revisions parent ON parent.id = r.parent_revision_id
  LEFT JOIN draft_revisions restored ON restored.id = r.restored_from_revision_id
`

function toSummary(row: VersionRow, routeCount = row.route_count): ConfigurationVersionSummary {
  return {
    id: bufferToPublicId(row.public_id),
    projectId: bufferToPublicId(row.project_public_id),
    number: row.revision_no,
    relation: row.revision_no === row.current_revision_no ? 'current' : 'historical',
    baseVersionId: row.parent_public_id ? bufferToPublicId(row.parent_public_id) : null,
    restoredFromVersionId: row.restored_from_public_id
      ? bufferToPublicId(row.restored_from_public_id)
      : null,
    changeSummary: row.change_summary,
    routeCount,
    modelSha256: row.plaintext_sha256.toString('hex'),
    validationState: row.validation_state,
    publicationStatus: row.publication_status ?? 'never',
    createdBy: {
      id: bufferToPublicId(row.creator_public_id),
      displayName: row.creator_display_name,
    },
    createdAt: mysqlDateTimeToRfc3339(row.created_at),
  }
}

export class ConfigurationVersionRepository {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, documents: DocumentRepository, audit = new AuditRepository()) {
    this.#pool = pool
    this.#documents = documents
    this.#audit = audit
  }

  async list(
    projectInternalId: string,
    beforeNumber: number | null,
    limit: number,
  ): Promise<readonly ConfigurationVersionSummary[]> {
    const values: (string | number)[] = [projectInternalId]
    const cursorClause = beforeNumber === null ? '' : 'AND r.revision_no < ?'
    if (beforeNumber !== null) values.push(beforeNumber)
    const [rows] = await this.#pool.execute<VersionRow[]>(
      `${selectVersions}
       WHERE p.id = ? ${cursorClause}
       ORDER BY r.revision_no DESC
       LIMIT ${limit}`,
      values,
    )
    return Promise.all(
      rows.map(async (row) =>
        toSummary(
          row,
          row.idempotency_key === null
            ? (await this.readModel(row.model_document_id)).routes.length
            : row.route_count,
        ),
      ),
    )
  }

  async findCurrentSummary(projectInternalId: string): Promise<ConfigurationVersionSummary | null> {
    const [rows] = await this.#pool.execute<VersionRow[]>(
      `${selectVersions}
       WHERE p.id = ? AND r.revision_no = d.current_revision_no
       LIMIT 1`,
      [projectInternalId],
    )
    if (!rows[0]) return null
    const row = rows[0]
    const routeCount =
      row.idempotency_key === null
        ? (await this.readModel(row.model_document_id)).routes.length
        : row.route_count
    return toSummary(row, routeCount)
  }

  async findDetail(
    projectInternalId: string,
    versionPublicId: string,
  ): Promise<ConfigurationVersionDetail | null> {
    const row = await this.findRow(projectInternalId, versionPublicId)
    if (!row) return null
    const model = await this.readModel(row.model_document_id)
    return { ...toSummary(row, model.routes.length), model }
  }

  async findStored(
    projectInternalId: string,
    versionPublicId: string,
  ): Promise<StoredConfigurationVersion | null> {
    const row = await this.findRow(projectInternalId, versionPublicId)
    if (!row) return null
    return {
      internalId: row.internal_id,
      draftInternalId: row.draft_internal_id,
      environmentInternalId: row.environment_internal_id,
      projectId: bufferToPublicId(row.project_public_id),
      projectInternalId,
      id: bufferToPublicId(row.public_id),
      number: row.revision_no,
      modelDocumentInternalId: row.model_document_id,
      modelSha256: row.plaintext_sha256.toString('hex'),
      model: await this.readModel(row.model_document_id),
    }
  }

  async save(input: SaveVersionRepositoryInput): Promise<SavedConfigurationVersion> {
    const plaintext = Buffer.from(canonicalJson(input.model), 'utf8')
    const encrypted = this.#documents.encrypt(plaintext)
    const requestDigest = sha256(
      canonicalJson({
        baseVersionId: input.baseVersionId,
        changeSummary: input.changeSummary,
        forceSameContent: input.forceSameContent,
        model: input.model,
        restoredFromVersionId: input.restoredFromVersionId ?? null,
      }),
    )
    const proposedPublicId = createPublicId()
    let savedPublicId = proposedPublicId
    let resultingLockVersion = input.expectedLockVersion

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [draftRows] = await transaction.execute<LockedDraftRow[]>(
          `SELECT id, environment_id, current_revision_no, lock_version
           FROM drafts
           WHERE project_id = ? AND archived_at IS NULL
           FOR UPDATE`,
          [input.project.id],
        )
        const draft = draftRows[0]
        if (!draft) throw notFound('Configuration workspace')

        const [replayRows] = await transaction.execute<
          (RowDataPacket & { public_id: Buffer; request_sha256: Buffer })[]
        >(
          `SELECT public_id, request_sha256
           FROM draft_revisions
           WHERE draft_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [draft.id, input.actor.internalId, input.idempotencyKey],
        )
        const replay = replayRows[0]
        if (replay) {
          if (!replay.request_sha256.equals(requestDigest)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with different version content',
            )
          }
          savedPublicId = bufferToPublicId(replay.public_id)
          resultingLockVersion = draft.lock_version
          return
        }

        if (draft.lock_version !== input.expectedLockVersion) {
          throw conflict(
            'CONFIG_VERSION_CONFLICT',
            'The current configuration version changed; reload and compare before saving',
          )
        }

        let current: CurrentVersionRow | null = null
        if (draft.current_revision_no > 0) {
          const [currentRows] = await transaction.execute<CurrentVersionRow[]>(
            `SELECT r.id, r.public_id, cd.plaintext_sha256
             FROM draft_revisions r
             INNER JOIN config_documents cd ON cd.id = r.model_document_id
             WHERE r.draft_id = ? AND r.revision_no = ?`,
            [draft.id, draft.current_revision_no],
          )
          current = currentRows[0] ?? null
        }
        const currentPublicId = current ? bufferToPublicId(current.public_id) : null
        if (currentPublicId !== input.baseVersionId) {
          throw conflict(
            'CONFIG_VERSION_CONFLICT',
            'The base configuration version is no longer current',
          )
        }
        if (
          current?.plaintext_sha256.equals(encrypted.plaintextSha256) &&
          !input.forceSameContent
        ) {
          throw conflict(
            'CONFIG_VERSION_UNCHANGED',
            'The route configuration is unchanged; confirm explicitly to save another version',
          )
        }

        let restoredFromInternalId: string | null = null
        if (input.restoredFromVersionId) {
          const [sourceRows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
            'SELECT id FROM draft_revisions WHERE draft_id = ? AND public_id = ?',
            [draft.id, publicIdToBuffer(input.restoredFromVersionId)],
          )
          restoredFromInternalId = sourceRows[0]?.id ?? null
          if (!restoredFromInternalId) throw notFound('Configuration version')
        }

        const document = await this.#documents.insert(transaction, {
          environmentInternalId: draft.environment_id,
          purpose: 'draft_model',
          contentType: 'application/json',
          schemaVersion: input.model.schemaVersion,
          encrypted,
        })
        const nextNumber = draft.current_revision_no + 1
        await transaction.execute<ResultSetHeader>(
          `INSERT INTO draft_revisions
            (public_id, draft_id, revision_no, parent_revision_id,
             restored_from_revision_id, model_document_id, validation_state, route_count,
             change_summary, idempotency_key, request_sha256, created_by)
           VALUES (?, ?, ?, ?, ?, ?, 'not_run', ?, ?, ?, ?, ?)`,
          [
            publicIdToBuffer(proposedPublicId),
            draft.id,
            nextNumber,
            current?.id ?? null,
            restoredFromInternalId,
            document.internalId,
            input.model.routes.length,
            input.changeSummary,
            input.idempotencyKey,
            requestDigest,
            input.actor.internalId,
          ],
        )
        const [update] = await transaction.execute<ResultSetHeader>(
          `UPDATE drafts
           SET current_revision_no = ?, lock_version = lock_version + 1,
               state = 'editing', updated_by = ?, updated_at = CURRENT_TIMESTAMP(6)
           WHERE id = ? AND lock_version = ?`,
          [nextNumber, input.actor.internalId, draft.id, input.expectedLockVersion],
        )
        if (update.affectedRows !== 1) {
          throw conflict('CONFIG_VERSION_CONFLICT', 'The configuration changed while saving')
        }
        resultingLockVersion = (BigInt(draft.lock_version) + 1n).toString()
        await this.#audit.append(transaction, {
          environmentInternalId: draft.environment_id,
          actorInternalId: input.actor.internalId,
          eventType: input.restoredFromVersionId
            ? 'configuration_version.restored'
            : 'configuration_version.created',
          targetType: 'configuration_version',
          targetPublicId: proposedPublicId,
          requestId: input.requestId,
          result: 'success',
          summary: {
            projectId: bufferToPublicId(input.project.public_id),
            number: nextNumber,
            baseVersionId: input.baseVersionId,
            restoredFromVersionId: input.restoredFromVersionId ?? null,
            modelSha256: document.sha256.toString('hex'),
            routeCount: input.model.routes.length,
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const version = await this.findDetail(input.project.id, savedPublicId)
    if (!version) throw new Error('Saved configuration version could not be reloaded')
    return { version, lockVersion: resultingLockVersion }
  }

  async recordValidation(
    actor: Actor,
    project: ProjectIdentityRow,
    version: StoredConfigurationVersion,
    validation: ProjectRoutesValidationView,
    requestId: string,
  ): Promise<void> {
    await withTransaction(this.#pool, async (transaction) => {
      await transaction.execute(
        `INSERT INTO validation_runs
          (public_id, draft_revision_id, model_sha256, stage, status, compiler_revision,
           validator_contract_version, validator_revision, errors_json, started_at, finished_at)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP(6), CURRENT_TIMESTAMP(6))`,
        [
          publicIdToBuffer(createPublicId()),
          version.internalId,
          Buffer.from(version.modelSha256, 'hex'),
          validation.validator ? 'native' : 'control',
          validation.valid ? 'valid' : 'invalid',
          ROUTE_COMPILER_REVISION,
          validation.validator?.contractVersion ?? null,
          validation.validator?.revision ?? null,
          JSON.stringify(validation.issues),
        ],
      )
      await transaction.execute('UPDATE draft_revisions SET validation_state = ? WHERE id = ?', [
        validation.valid ? 'valid' : 'invalid',
        version.internalId,
      ])
      await this.#audit.append(transaction, {
        environmentInternalId: project.environment_id,
        actorInternalId: actor.internalId,
        eventType: 'configuration_version.validation_completed',
        targetType: 'configuration_version',
        targetPublicId: version.id,
        requestId,
        result: validation.valid ? 'success' : 'failure',
        summary: {
          projectId: version.projectId,
          number: version.number,
          valid: validation.valid,
          issueCount: validation.issues.length,
          modelSha256: version.modelSha256,
          validatorRevision: validation.validator?.revision ?? null,
        },
      })
    })
  }

  async lockVersion(projectInternalId: string): Promise<string> {
    const [rows] = await this.#pool.execute<(RowDataPacket & { lock_version: string })[]>(
      'SELECT lock_version FROM drafts WHERE project_id = ? AND archived_at IS NULL',
      [projectInternalId],
    )
    if (!rows[0]) throw notFound('Configuration workspace')
    return rows[0].lock_version
  }

  private async findRow(
    projectInternalId: string,
    versionPublicId: string,
  ): Promise<VersionRow | null> {
    const [rows] = await this.#pool.execute<VersionRow[]>(
      `${selectVersions}
       WHERE p.id = ? AND r.public_id = ?
       LIMIT 1`,
      [projectInternalId, publicIdToBuffer(versionPublicId)],
    )
    return rows[0] ?? null
  }

  private async readModel(documentInternalId: string): Promise<ProjectRoutesModel> {
    const plaintext = await this.#documents.decryptByInternalId(this.#pool, documentInternalId)
    if (!plaintext) throw new Error('Configuration version document was not found')
    const parsed: unknown = JSON.parse(plaintext.toString('utf8'))
    const model = normalizeStoredProjectRoutesModel(parsed)
    if (!model) throw new Error('Stored configuration version is invalid')
    return model
  }
}
