// Handwritten edge-function draft persistence. Mirrors the WAF policy draft
// repository (repository.ts): one draft per environment, functions JSON
// envelope-encrypted into config_documents (purpose 'edge_functions_draft'),
// every save appends an immutable document row and repoints
// functions_document_id, lock_version drives optimistic concurrency.

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
import { emptyEdgeFunctionsDraft, type EdgeFunctionsDraft } from './edge-function-model.js'
import { validateEdgeFunctionsDraft } from './edge-function-validate.js'

export interface EdgeFunctionDraftView {
  id: string
  draft: EdgeFunctionsDraft
  draftSha256: string
  lockVersion: string
  createdAt: string
  updatedAt: string
}

const EDGE_FUNCTIONS_DRAFT_PURPOSE = 'edge_functions_draft'

interface EdgeFunctionDraftRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  lock_version: string
  functions_document_id: string
  plaintext_sha256: Buffer
  created_at: string
  updated_at: string
}

interface LockedDraftRow extends RowDataPacket {
  id: string
  public_id: Buffer
  environment_id: string
  lock_version: string
}

const selectDraft = `
  SELECT d.id AS internal_id, d.public_id, d.lock_version, d.functions_document_id,
         cd.plaintext_sha256, d.created_at, d.updated_at
  FROM edge_function_drafts d
  INNER JOIN config_documents cd ON cd.id = d.functions_document_id
`

export class EdgeFunctionDraftRepository {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, documents: DocumentRepository, audit = new AuditRepository()) {
    this.#pool = pool
    this.#documents = documents
    this.#audit = audit
  }

  async findByEnvironmentInternalId(
    environmentInternalId: string,
  ): Promise<EdgeFunctionDraftView | null> {
    const [rows] = await this.#pool.execute<EdgeFunctionDraftRow[]>(
      `${selectDraft} WHERE d.environment_id = ?`,
      [environmentInternalId],
    )
    return rows[0] ? await this.#toView(rows[0]) : null
  }

  async getOrCreate(
    actor: Actor,
    environmentInternalId: string,
    requestId: string,
  ): Promise<EdgeFunctionDraftView> {
    const existing = await this.findByEnvironmentInternalId(environmentInternalId)
    if (existing) {
      return existing
    }

    const publicId = createPublicId()
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          const document = await this.#documents.insert(transaction, {
            environmentInternalId,
            purpose: EDGE_FUNCTIONS_DRAFT_PURPOSE,
            contentType: 'application/json',
            schemaVersion: emptyEdgeFunctionsDraft().schemaVersion,
            encrypted: this.#documents.encrypt(
              Buffer.from(canonicalJson(emptyEdgeFunctionsDraft()), 'utf8'),
            ),
          })
          await transaction.execute<ResultSetHeader>(
            `INSERT INTO edge_function_drafts
              (public_id, environment_id, state, lock_version, functions_document_id,
               created_by, updated_by)
             VALUES (?, ?, 'editing', 0, ?, ?, ?)`,
            [
              publicIdToBuffer(publicId),
              environmentInternalId,
              document.internalId,
              actor.internalId,
              actor.internalId,
            ],
          )
          await this.#audit.append(transaction, {
            environmentInternalId,
            actorInternalId: actor.internalId,
            eventType: 'edge_function_draft.created',
            targetType: 'edge_function_draft',
            targetPublicId: publicId,
            requestId,
            result: 'success',
            summary: {},
          })
        },
        { retryOnDeadlock: true },
      )
    } catch (error) {
      // The unique environment key makes concurrent get-or-create safe.
      const winner = await this.findByEnvironmentInternalId(environmentInternalId)
      if (winner) {
        return winner
      }
      throw error
    }

    const created = await this.findByEnvironmentInternalId(environmentInternalId)
    if (!created) {
      throw new Error('Created edge-function draft could not be reloaded')
    }
    return created
  }

  async save(
    actor: Actor,
    environmentInternalId: string,
    expectedLockVersion: string,
    draft: EdgeFunctionsDraft,
    requestId: string,
  ): Promise<EdgeFunctionDraftView> {
    const plaintext = Buffer.from(canonicalJson(draft), 'utf8')
    const encrypted = this.#documents.encrypt(plaintext)

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [rows] = await transaction.execute<LockedDraftRow[]>(
          `SELECT id, public_id, environment_id, lock_version
           FROM edge_function_drafts
           WHERE environment_id = ?
           FOR UPDATE`,
          [environmentInternalId],
        )
        const row = rows[0]
        if (!row) {
          throw notFound('Edge function draft')
        }
        if (row.lock_version !== expectedLockVersion) {
          throw conflict(
            'EDGE_FUNCTION_DRAFT_VERSION_CONFLICT',
            'The edge-function draft changed; reload it before saving',
          )
        }

        const document = await this.#documents.insert(transaction, {
          environmentInternalId: row.environment_id,
          purpose: EDGE_FUNCTIONS_DRAFT_PURPOSE,
          contentType: 'application/json',
          schemaVersion: draft.schemaVersion,
          encrypted,
        })
        const [update] = await transaction.execute<ResultSetHeader>(
          `UPDATE edge_function_drafts
           SET functions_document_id = ?, lock_version = lock_version + 1,
               updated_by = ?, updated_at = CURRENT_TIMESTAMP(6)
           WHERE id = ? AND lock_version = ?`,
          [document.internalId, actor.internalId, row.id, expectedLockVersion],
        )
        if (update.affectedRows !== 1) {
          throw conflict(
            'EDGE_FUNCTION_DRAFT_VERSION_CONFLICT',
            'The edge-function draft changed; reload it before saving',
          )
        }
        await this.#audit.append(transaction, {
          environmentInternalId: row.environment_id,
          actorInternalId: actor.internalId,
          eventType: 'edge_function_draft.updated',
          targetType: 'edge_function_draft',
          targetPublicId: bufferToPublicId(row.public_id),
          requestId,
          result: 'success',
          summary: {
            draftSha256: document.sha256.toString('hex'),
            functions: draft.functions.length,
            enabled: draft.functions.filter((fn) => fn.enabled).length,
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const saved = await this.findByEnvironmentInternalId(environmentInternalId)
    if (!saved) {
      throw new Error('Saved edge-function draft could not be reloaded')
    }
    return saved
  }

  async #toView(row: EdgeFunctionDraftRow): Promise<EdgeFunctionDraftView> {
    const plaintext = await this.#documents.decryptByInternalId(
      this.#pool,
      row.functions_document_id,
    )
    if (!plaintext) {
      throw new Error('Edge function draft document was not found')
    }
    const parsed: unknown = JSON.parse(plaintext.toString('utf8'))
    if (validateEdgeFunctionsDraft(parsed).length > 0) {
      throw new Error('Stored edge-function draft is invalid')
    }
    return {
      id: bufferToPublicId(row.public_id),
      draft: parsed as EdgeFunctionsDraft,
      draftSha256: row.plaintext_sha256.toString('hex'),
      lockVersion: row.lock_version,
      createdAt: mysqlDateTimeToRfc3339(row.created_at),
      updatedAt: mysqlDateTimeToRfc3339(row.updated_at),
    }
  }
}
