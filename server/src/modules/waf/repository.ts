// WAF policy draft persistence (docs/edge-functions-and-waf-design.md §4.3).
//
// One draft per environment: the WAF policy is a workspace-wide authoring
// model, not per-project. The policy JSON is envelope-encrypted into
// config_documents (purpose 'waf_policy'); every save inserts a new immutable
// document row and repoints policy_document_id, so historical draft content
// stays readable without a revision table. lock_version drives optimistic
// concurrency exactly like project drafts.

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
import { emptyWafPolicy, type WafPolicy } from './model.js'
import { upgradeWafPolicy } from './policy-upgrade.js'
import { validateWafPolicy } from './validate.js'

export interface WafPolicyDraftView {
  id: string
  policy: WafPolicy
  policySha256: string
  lockVersion: string
  createdAt: string
  updatedAt: string
}

const WAF_POLICY_PURPOSE = 'waf_policy'

interface WafPolicyDraftRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  lock_version: string
  policy_document_id: string
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
  SELECT d.id AS internal_id, d.public_id, d.lock_version, d.policy_document_id,
         cd.plaintext_sha256, d.created_at, d.updated_at
  FROM waf_policy_drafts d
  INNER JOIN config_documents cd ON cd.id = d.policy_document_id
`

export class WafPolicyDraftRepository {
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
  ): Promise<WafPolicyDraftView | null> {
    const [rows] = await this.#pool.execute<WafPolicyDraftRow[]>(
      `${selectDraft} WHERE d.environment_id = ?`,
      [environmentInternalId],
    )
    return rows[0] ? await this.#toView(rows[0]) : null
  }

  async getOrCreate(
    actor: Actor,
    environmentInternalId: string,
    requestId: string,
  ): Promise<WafPolicyDraftView> {
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
            purpose: WAF_POLICY_PURPOSE,
            contentType: 'application/json',
            schemaVersion: emptyWafPolicy().schemaVersion,
            encrypted: this.#documents.encrypt(
              Buffer.from(canonicalJson(emptyWafPolicy()), 'utf8'),
            ),
          })
          await transaction.execute<ResultSetHeader>(
            `INSERT INTO waf_policy_drafts
              (public_id, environment_id, state, lock_version, policy_document_id,
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
            eventType: 'waf_policy_draft.created',
            targetType: 'waf_policy_draft',
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
      throw new Error('Created WAF policy draft could not be reloaded')
    }
    return created
  }

  async save(
    actor: Actor,
    environmentInternalId: string,
    expectedLockVersion: string,
    policy: WafPolicy,
    requestId: string,
  ): Promise<WafPolicyDraftView> {
    const plaintext = Buffer.from(canonicalJson(policy), 'utf8')
    const encrypted = this.#documents.encrypt(plaintext)

    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [rows] = await transaction.execute<LockedDraftRow[]>(
          `SELECT id, public_id, environment_id, lock_version
           FROM waf_policy_drafts
           WHERE environment_id = ?
           FOR UPDATE`,
          [environmentInternalId],
        )
        const draft = rows[0]
        if (!draft) {
          throw notFound('WAF policy draft')
        }
        if (draft.lock_version !== expectedLockVersion) {
          throw conflict(
            'WAF_POLICY_VERSION_CONFLICT',
            'The WAF policy changed; reload it before saving',
          )
        }

        const document = await this.#documents.insert(transaction, {
          environmentInternalId: draft.environment_id,
          purpose: WAF_POLICY_PURPOSE,
          contentType: 'application/json',
          schemaVersion: policy.schemaVersion,
          encrypted,
        })
        const [update] = await transaction.execute<ResultSetHeader>(
          `UPDATE waf_policy_drafts
           SET policy_document_id = ?, lock_version = lock_version + 1,
               updated_by = ?, updated_at = CURRENT_TIMESTAMP(6)
           WHERE id = ? AND lock_version = ?`,
          [document.internalId, actor.internalId, draft.id, expectedLockVersion],
        )
        if (update.affectedRows !== 1) {
          throw conflict(
            'WAF_POLICY_VERSION_CONFLICT',
            'The WAF policy changed; reload it before saving',
          )
        }
        await this.#audit.append(transaction, {
          environmentInternalId: draft.environment_id,
          actorInternalId: actor.internalId,
          eventType: 'waf_policy_draft.updated',
          targetType: 'waf_policy_draft',
          targetPublicId: bufferToPublicId(draft.public_id),
          requestId,
          result: 'success',
          summary: {
            policySha256: document.sha256.toString('hex'),
            groups: policy.groups.length,
            rules: policy.rules.length,
          },
        })
      },
      { retryOnDeadlock: true },
    )

    const saved = await this.findByEnvironmentInternalId(environmentInternalId)
    if (!saved) {
      throw new Error('Saved WAF policy draft could not be reloaded')
    }
    return saved
  }

  async #toView(row: WafPolicyDraftRow): Promise<WafPolicyDraftView> {
    const plaintext = await this.#documents.decryptByInternalId(this.#pool, row.policy_document_id)
    if (!plaintext) {
      throw new Error('WAF policy document was not found')
    }
    const parsed: unknown = JSON.parse(plaintext.toString('utf8'))
    const policy =
      Array.isArray(parsed) || typeof parsed !== 'object' || parsed === null
        ? null
        : upgradeWafPolicy(parsed)
    if (!policy || validateWafPolicy(policy as WafPolicy).length > 0) {
      throw new Error('Stored WAF policy is invalid')
    }
    return {
      id: bufferToPublicId(row.public_id),
      policy: policy as WafPolicy,
      policySha256: row.plaintext_sha256.toString('hex'),
      lockVersion: row.lock_version,
      createdAt: mysqlDateTimeToRfc3339(row.created_at),
      updatedAt: mysqlDateTimeToRfc3339(row.updated_at),
    }
  }
}
