// Environment-wide gray rules draft persistence.
//
// One draft per environment, envelope-encrypted into config_documents (purpose
// 'gray_rules'). Every save inserts a new immutable document row and updates
// rules_document_id with lock_version optimistic concurrency.

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
import { emptyGrayRulesDocument, type GrayRulesDocument } from './model.js'
import { validateGrayRules } from './validate.js'

export interface GrayRulesDraftView {
  id: string
  document: GrayRulesDocument
  documentSha256: string
  lockVersion: string
  createdAt: string
  updatedAt: string
}

const GRAY_RULES_PURPOSE = 'gray_rules'

interface GrayRuleDraftRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  lock_version: string
  rules_document_id: string
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
  SELECT d.id AS internal_id, d.public_id, d.lock_version, d.rules_document_id,
         cd.plaintext_sha256, d.created_at, d.updated_at
  FROM gray_rule_drafts d
  INNER JOIN config_documents cd ON cd.id = d.rules_document_id
`

export class GrayRuleDraftRepository {
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
  ): Promise<GrayRulesDraftView | null> {
    const [rows] = await this.#pool.execute<GrayRuleDraftRow[]>(
      `${selectDraft} WHERE d.environment_id = ?`,
      [environmentInternalId],
    )
    return rows[0] ? await this.#toView(rows[0]) : null
  }

  async getOrCreate(
    actor: Actor,
    environmentInternalId: string,
    requestId: string,
  ): Promise<GrayRulesDraftView> {
    const existing = await this.findByEnvironmentInternalId(environmentInternalId)
    if (existing) {
      return existing
    }

    const publicId = createPublicId()
    const initialDocument = emptyGrayRulesDocument()
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          const document = await this.#documents.insert(transaction, {
            environmentInternalId,
            purpose: GRAY_RULES_PURPOSE,
            contentType: 'application/json',
            schemaVersion: initialDocument.schemaVersion,
            encrypted: this.#documents.encrypt(Buffer.from(canonicalJson(initialDocument), 'utf8')),
          })
          await transaction.execute<ResultSetHeader>(
            `INSERT INTO gray_rule_drafts
              (public_id, environment_id, state, lock_version, rules_document_id,
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
            eventType: 'gray_rule_draft.created',
            targetType: 'gray_rule_draft',
            targetPublicId: publicId,
            requestId,
            result: 'success',
            summary: {},
          })
        },
        { retryOnDeadlock: true },
      )
    } catch (error) {
      const winner = await this.findByEnvironmentInternalId(environmentInternalId)
      if (winner) {
        return winner
      }
      throw error
    }

    const created = await this.findByEnvironmentInternalId(environmentInternalId)
    if (!created) {
      throw new Error('Created gray rule draft could not be reloaded')
    }
    return created
  }

  async save(
    actor: Actor,
    environmentInternalId: string,
    expectedLockVersion: string,
    document: GrayRulesDocument,
    requestId: string,
  ): Promise<GrayRulesDraftView> {
    const plaintext = Buffer.from(canonicalJson(document), 'utf8')
    const encrypted = this.#documents.encrypt(plaintext)

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [rows] = await transaction.execute<LockedDraftRow[]>(
          `SELECT id, public_id, environment_id, lock_version
           FROM gray_rule_drafts
           WHERE environment_id = ?
           FOR UPDATE`,
          [environmentInternalId],
        )
        const draft = rows[0]
        if (!draft) {
          throw notFound('Gray rules draft')
        }
        if (draft.lock_version !== expectedLockVersion) {
          throw conflict(
            'GRAY_RULES_VERSION_CONFLICT',
            'The gray rules draft changed; reload it before saving',
          )
        }

        const stored = await this.#documents.insert(transaction, {
          environmentInternalId: draft.environment_id,
          purpose: GRAY_RULES_PURPOSE,
          contentType: 'application/json',
          schemaVersion: document.schemaVersion,
          encrypted,
        })
        const [update] = await transaction.execute<ResultSetHeader>(
          `UPDATE gray_rule_drafts
           SET rules_document_id = ?, lock_version = lock_version + 1,
               updated_by = ?, updated_at = CURRENT_TIMESTAMP(6)
           WHERE id = ? AND lock_version = ?`,
          [stored.internalId, actor.internalId, draft.id, expectedLockVersion],
        )
        if (update.affectedRows !== 1) {
          throw conflict(
            'GRAY_RULES_VERSION_CONFLICT',
            'The gray rules draft changed; reload it before saving',
          )
        }
        await this.#audit.append(transaction, {
          environmentInternalId: draft.environment_id,
          actorInternalId: actor.internalId,
          eventType: 'gray_rule_draft.updated',
          targetType: 'gray_rule_draft',
          targetPublicId: bufferToPublicId(draft.public_id),
          requestId,
          result: 'success',
          summary: {
            documentSha256: stored.sha256.toString('hex'),
            ruleCount: document.rules.length,
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const saved = await this.findByEnvironmentInternalId(environmentInternalId)
    if (!saved) {
      throw new Error('Saved gray rule draft could not be reloaded')
    }
    return saved
  }

  async #toView(row: GrayRuleDraftRow): Promise<GrayRulesDraftView> {
    const plaintext = await this.#documents.decryptByInternalId(this.#pool, row.rules_document_id)
    if (!plaintext) {
      throw new Error('Gray rules document was not found')
    }
    const parsed: unknown = JSON.parse(plaintext.toString('utf8'))
    if (validateGrayRules(parsed).length > 0) {
      throw new Error('Stored gray rules document is invalid')
    }
    return {
      id: bufferToPublicId(row.public_id),
      document: parsed as GrayRulesDocument,
      documentSha256: row.plaintext_sha256.toString('hex'),
      lockVersion: row.lock_version,
      createdAt: mysqlDateTimeToRfc3339(row.created_at),
      updatedAt: mysqlDateTimeToRfc3339(row.updated_at),
    }
  }
}
