import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import { conflict, notFound } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { canonicalJson } from '../../shared/json.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type { ProjectIdentityRow } from '../projects/repository.js'
import {
  normalizeStoredProjectRoutesModel,
  type DraftRevisionView,
  type DraftView,
  type ProjectRoutesModel,
} from './model.js'

interface DraftRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  project_public_id: Buffer
  state: DraftView['state']
  title: string
  current_revision_no: number
  lock_version: string
  created_at: string
  updated_at: string
}

interface LockedDraftRow extends RowDataPacket {
  id: string
  public_id: Buffer
  environment_id: string
  project_id: string
  current_revision_no: number
  lock_version: string
}

interface RevisionRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  draft_public_id: Buffer
  model_document_id: string
  plaintext_sha256: Buffer
  revision_no: number
  validation_state: DraftRevisionView['validationState']
  change_summary: string
  created_at: string
  project_name: string
}

function toDraftView(row: DraftRow): DraftView {
  return {
    id: bufferToPublicId(row.public_id),
    projectId: bufferToPublicId(row.project_public_id),
    state: row.state,
    title: row.title,
    currentRevision: row.current_revision_no,
    lockVersion: row.lock_version,
    createdAt: mysqlDateTimeToRfc3339(row.created_at),
    updatedAt: mysqlDateTimeToRfc3339(row.updated_at),
  }
}

const selectDraft = `
  SELECT d.id AS internal_id, d.public_id, p.public_id AS project_public_id,
         d.state, d.title, d.current_revision_no, d.lock_version,
         d.created_at, d.updated_at
  FROM drafts d
  INNER JOIN projects p ON p.id = d.project_id
`

export class DraftRepository {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, documents: DocumentRepository, audit = new AuditRepository()) {
    this.#pool = pool
    this.#documents = documents
    this.#audit = audit
  }

  async getOrCreate(
    actor: Actor,
    project: ProjectIdentityRow,
    requestId: string,
  ): Promise<DraftView> {
    const existing = await this.findByProjectInternalId(project.id)
    if (existing) {
      return existing
    }

    const publicId = createPublicId()
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          await transaction.execute(
            `INSERT INTO drafts
              (public_id, environment_id, project_id, scope_key, kind, state, title,
               created_by, updated_by)
             VALUES (?, ?, ?, ?, 'project_route', 'editing', ?, ?, ?)`,
            [
              publicIdToBuffer(publicId),
              project.environment_id,
              project.id,
              `project:${project.id}`,
              `${project.name} configuration`,
              actor.internalId,
              actor.internalId,
            ],
          )
          await this.#audit.append(transaction, {
            environmentInternalId: project.environment_id,
            actorInternalId: actor.internalId,
            eventType: 'draft.created',
            targetType: 'draft',
            targetPublicId: publicId,
            requestId,
            result: 'success',
            summary: { projectId: bufferToPublicId(project.public_id) },
          })
        },
        { retryOnDeadlock: true },
      )
    } catch (error) {
      // The unique environment/scope key makes concurrent get-or-create safe.
      const winner = await this.findByProjectInternalId(project.id)
      if (winner) {
        return winner
      }
      throw error
    }

    const created = await this.findByPublicId(publicId)
    if (!created) {
      throw new Error('Created draft could not be reloaded')
    }
    return created
  }

  async findByPublicId(publicId: string): Promise<DraftView | null> {
    const [rows] = await this.#pool.execute<DraftRow[]>(
      `${selectDraft} WHERE d.public_id = ? AND d.archived_at IS NULL`,
      [publicIdToBuffer(publicId)],
    )
    return rows[0] ? toDraftView(rows[0]) : null
  }

  async findByProjectInternalId(projectInternalId: string): Promise<DraftView | null> {
    const [rows] = await this.#pool.execute<DraftRow[]>(
      `${selectDraft} WHERE d.project_id = ? AND d.archived_at IS NULL`,
      [projectInternalId],
    )
    return rows[0] ? toDraftView(rows[0]) : null
  }

  async createRevision(
    actor: Actor,
    draftPublicId: string,
    expectedLockVersion: string,
    model: ProjectRoutesModel,
    changeSummary: string,
    requestId: string,
  ): Promise<DraftRevisionView> {
    const plaintext = Buffer.from(canonicalJson(model), 'utf8')
    const encrypted = this.#documents.encrypt(plaintext)
    const revisionPublicId = createPublicId()

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [identityRows] = await transaction.execute<
          (RowDataPacket & { project_id: string })[]
        >(
          `SELECT project_id
           FROM drafts
           WHERE public_id = ? AND archived_at IS NULL`,
          [publicIdToBuffer(draftPublicId)],
        )
        const identity = identityRows[0]
        if (!identity) throw notFound('Draft')
        const [projectRows] = await transaction.execute<(RowDataPacket & { status: string })[]>(
          'SELECT status FROM projects WHERE id = ? FOR UPDATE',
          [identity.project_id],
        )
        if (projectRows[0]?.status !== 'active') {
          throw conflict('PROJECT_NOT_ACTIVE', 'Drafts can only be saved for an active Project')
        }
        const [rows] = await transaction.execute<LockedDraftRow[]>(
          `SELECT id, public_id, environment_id, project_id, current_revision_no, lock_version
           FROM drafts
           WHERE public_id = ? AND archived_at IS NULL
           FOR UPDATE`,
          [publicIdToBuffer(draftPublicId)],
        )
        const draft = rows[0]
        if (!draft) {
          throw notFound('Draft')
        }
        if (draft.lock_version !== expectedLockVersion) {
          throw conflict('DRAFT_VERSION_CONFLICT', 'The draft changed; reload it before saving')
        }

        const parentRevisionNo = draft.current_revision_no
        const nextRevisionNo = parentRevisionNo + 1
        let parentRevisionId: string | null = null
        if (parentRevisionNo > 0) {
          const [parentRows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
            'SELECT id FROM draft_revisions WHERE draft_id = ? AND revision_no = ?',
            [draft.id, parentRevisionNo],
          )
          parentRevisionId = parentRows[0]?.id ?? null
        }

        const document = await this.#documents.insert(transaction, {
          environmentInternalId: draft.environment_id,
          purpose: 'draft_model',
          contentType: 'application/json',
          schemaVersion: model.schemaVersion,
          encrypted,
        })
        await transaction.execute<ResultSetHeader>(
          `INSERT INTO draft_revisions
            (public_id, draft_id, revision_no, parent_revision_id, model_document_id,
             validation_state, change_summary, created_by)
           VALUES (?, ?, ?, ?, ?, 'not_run', ?, ?)`,
          [
            publicIdToBuffer(revisionPublicId),
            draft.id,
            nextRevisionNo,
            parentRevisionId,
            document.internalId,
            changeSummary,
            actor.internalId,
          ],
        )
        const [update] = await transaction.execute<ResultSetHeader>(
          `UPDATE drafts
           SET current_revision_no = ?, lock_version = lock_version + 1,
               state = 'editing', updated_by = ?, updated_at = CURRENT_TIMESTAMP(6)
           WHERE id = ? AND lock_version = ?`,
          [nextRevisionNo, actor.internalId, draft.id, expectedLockVersion],
        )
        if (update.affectedRows !== 1) {
          throw conflict('DRAFT_VERSION_CONFLICT', 'The draft changed; reload it before saving')
        }
        await this.#audit.append(transaction, {
          environmentInternalId: draft.environment_id,
          actorInternalId: actor.internalId,
          eventType: 'draft.revision_created',
          targetType: 'draft_revision',
          targetPublicId: revisionPublicId,
          requestId,
          result: 'success',
          summary: {
            draftId: draftPublicId,
            revision: nextRevisionNo,
            modelSha256: document.sha256.toString('hex'),
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const created = await this.getRevision(draftPublicId, revisionPublicId)
    if (!created) {
      throw new Error('Created draft revision could not be reloaded')
    }
    return created
  }

  async getRevision(
    draftPublicId: string,
    revisionPublicId: string,
  ): Promise<DraftRevisionView | null> {
    const [rows] = await this.#pool.execute<RevisionRow[]>(
      `SELECT r.id AS internal_id, r.public_id, d.public_id AS draft_public_id,
              r.model_document_id, cd.plaintext_sha256, r.revision_no,
              r.validation_state, r.change_summary, r.created_at, p.name AS project_name
       FROM draft_revisions r
       INNER JOIN drafts d ON d.id = r.draft_id
       INNER JOIN projects p ON p.id = d.project_id
       INNER JOIN config_documents cd ON cd.id = r.model_document_id
       WHERE d.public_id = ? AND r.public_id = ? AND d.archived_at IS NULL`,
      [publicIdToBuffer(draftPublicId), publicIdToBuffer(revisionPublicId)],
    )
    const row = rows[0]
    if (!row) {
      return null
    }
    const plaintext = await this.#documents.decryptByInternalId(this.#pool, row.model_document_id)
    if (!plaintext) {
      throw new Error('Draft model document was not found')
    }
    const parsed: unknown = JSON.parse(plaintext.toString('utf8'))
    const model = normalizeStoredProjectRoutesModel(parsed, undefined, row.project_name)
    if (!model) {
      throw new Error('Stored draft model is invalid')
    }
    return {
      id: bufferToPublicId(row.public_id),
      draftId: bufferToPublicId(row.draft_public_id),
      revision: row.revision_no,
      model,
      modelSha256: row.plaintext_sha256.toString('hex'),
      validationState: row.validation_state,
      changeSummary: row.change_summary,
      createdAt: mysqlDateTimeToRfc3339(row.created_at),
    }
  }

  async getCurrentRevision(draftPublicId: string): Promise<DraftRevisionView | null> {
    const [rows] = await this.#pool.execute<(RowDataPacket & { public_id: Buffer })[]>(
      `SELECT r.public_id
       FROM draft_revisions r
       INNER JOIN drafts d ON d.id = r.draft_id
       WHERE d.public_id = ? AND d.archived_at IS NULL
         AND r.revision_no = d.current_revision_no
       LIMIT 1`,
      [publicIdToBuffer(draftPublicId)],
    )
    return rows[0] ? this.getRevision(draftPublicId, bufferToPublicId(rows[0].public_id)) : null
  }
}
