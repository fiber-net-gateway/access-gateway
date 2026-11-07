import assert from 'node:assert/strict'
import test from 'node:test'

import { createHash } from 'node:crypto'

import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import type { DatabasePool } from '../../database/types.js'
import { AppError } from '../../shared/errors.js'
import { publicIdToBuffer } from '../../shared/ids.js'
import { canonicalJson } from '../../shared/json.js'
import { EdgeFunctionDraftRepository } from './edge-function-repository.js'
import type { EdgeFunctionsDraft } from './edge-function-model.js'
import { emptyEdgeFunctionsDraft } from './edge-function-model.js'

const actor = {
  internalId: '1',
  publicId: 'p',
  subject: 's',
  displayName: 'd',
  platformAdmin: false,
}
const environmentInternalId = '11'
const draftPublicId = '00000000-0000-4000-8000-00000000000d'

function sampleDraft(): EdgeFunctionsDraft {
  return {
    schemaVersion: 1,
    functions: [
      {
        name: 'bot-trap',
        description: 'first handwritten function',
        enabled: true,
        script: 'return true;',
        mounts: [
          { projects: { include: ['loan-*'], exclude: ['loan-admin-legacy'] }, priority: 200 },
        ],
      },
    ],
  }
}

function buildPool(dispatch: (sql: string) => unknown): {
  pool: DatabasePool
  statements: string[]
} {
  const statements: string[] = []
  const connection = {
    query: async () => [[], []],
    beginTransaction: async () => undefined,
    commit: async () => undefined,
    rollback: async () => undefined,
    release: () => undefined,
    execute: async (sql: string) => {
      statements.push(sql)
      const rows = dispatch(sql)
      if (rows === undefined) throw new Error(`Unexpected SQL: ${sql}`)
      return [rows, []] as unknown as [ResultSetHeader, unknown[]]
    },
  }
  const pool = {
    getConnection: async () => connection,
    execute: async (sql: string) => {
      statements.push(sql)
      const rows = dispatch(sql)
      if (rows === undefined) throw new Error(`Unexpected SQL: ${sql}`)
      return [rows, []] as unknown as [ResultSetHeader, unknown[]]
    },
  } as unknown as DatabasePool
  return { pool, statements }
}

function documentsFake(documents: Map<string, string>) {
  let nextId = 100
  const state = { lastInsertedId: '' }
  const fake = {
    state,
    encrypt: (plaintext: Uint8Array) => ({
      plaintextSha256: createHash('sha256').update(plaintext).digest(),
      plaintextSize: plaintext.byteLength,
      keyId: 'key-1',
      wrappedDek: Buffer.alloc(16),
      nonce: Buffer.alloc(12),
      authTag: Buffer.alloc(16),
      ciphertext: Buffer.from(plaintext),
    }),
    insert: async (
      _executor: unknown,
      input: { encrypted: { ciphertext: Buffer; plaintextSha256: Buffer; plaintextSize: number } },
    ) => {
      const internalId = `doc-${nextId}`
      nextId += 1
      documents.set(internalId, input.encrypted.ciphertext.toString('utf8'))
      state.lastInsertedId = internalId
      return {
        internalId,
        publicId: internalId,
        sha256: input.encrypted.plaintextSha256,
        size: input.encrypted.plaintextSize,
      }
    },
    decryptByInternalId: async (_executor: unknown, internalId: string) => {
      const text = documents.get(internalId)
      return text === undefined ? null : Buffer.from(text, 'utf8')
    },
  }
  return fake
}

function draftRow(overrides: Record<string, unknown> = {}): RowDataPacket {
  return {
    internal_id: '5',
    id: '5',
    environment_id: environmentInternalId,
    public_id: publicIdToBuffer(draftPublicId),
    lock_version: '2',
    functions_document_id: 'doc-1',
    plaintext_sha256: Buffer.alloc(32, 7),
    created_at: '2026-11-07 00:00:00.000000',
    updated_at: '2026-11-07 00:00:00.000000',
    ...overrides,
  } as RowDataPacket
}

test('findByEnvironmentInternalId decrypts and validates the stored draft', async () => {
  const documents = documentsFake(new Map([['doc-1', canonicalJson(sampleDraft())]]))
  const { pool } = buildPool((sql) => {
    if (sql.includes('FROM edge_function_drafts')) return [draftRow()]
    return undefined
  })
  const repository = new EdgeFunctionDraftRepository(pool, documents as never)

  const view = await repository.findByEnvironmentInternalId(environmentInternalId)

  assert.equal(view?.id, draftPublicId)
  assert.equal(view?.lockVersion, '2')
  assert.deepEqual(view?.draft, sampleDraft())
})

test('findByEnvironmentInternalId rejects a stored draft that fails validation', async () => {
  const documents = documentsFake(
    new Map([['doc-1', canonicalJson({ schemaVersion: 1, functions: [{ name: 'x' }] })]]),
  )
  const { pool } = buildPool((sql) => {
    if (sql.includes('FROM edge_function_drafts')) return [draftRow()]
    return undefined
  })
  const repository = new EdgeFunctionDraftRepository(pool, documents as never)

  await assert.rejects(
    repository.findByEnvironmentInternalId(environmentInternalId),
    /Stored edge-function draft is invalid/,
  )
})

test('findByEnvironmentInternalId still loads a draft whose name the WAF later reserved', async () => {
  const reserved = {
    ...sampleDraft(),
    functions: [{ ...sampleDraft().functions[0]!, name: 'waf-candidate' }],
  }
  const documents = documentsFake(new Map([['doc-1', canonicalJson(reserved)]]))
  const { pool } = buildPool((sql) => {
    if (sql.includes('FROM edge_function_drafts')) return [draftRow()]
    return undefined
  })
  const repository = new EdgeFunctionDraftRepository(pool, documents as never)

  const view = await repository.findByEnvironmentInternalId(environmentInternalId)

  assert.equal(view?.draft.functions[0]?.name, 'waf-candidate')
})

test('save refuses a stale lock version before touching documents', async () => {
  const documents = documentsFake(new Map([['doc-1', canonicalJson(sampleDraft())]]))
  const { pool } = buildPool((sql) => {
    if (sql.includes('FOR UPDATE')) return [draftRow({ lock_version: '5' })]
    return undefined
  })
  const repository = new EdgeFunctionDraftRepository(pool, documents as never)

  await assert.rejects(
    repository.save(actor, environmentInternalId, '2', sampleDraft(), 'request-1'),
    (error: unknown) => {
      assert.ok(error instanceof AppError)
      assert.equal(error.code, 'EDGE_FUNCTION_DRAFT_VERSION_CONFLICT')
      return true
    },
  )
})

test('save inserts a document, repoints the draft, and audits', async () => {
  const documents = documentsFake(new Map([['doc-1', canonicalJson(sampleDraft())]]))
  const events: string[] = []
  let lockVersion = '2'
  let functionsDocumentId = 'doc-1'
  const { pool, statements } = buildPool((sql) => {
    if (sql.includes('FOR UPDATE')) return [draftRow({ lock_version: lockVersion })]
    if (sql.startsWith('UPDATE edge_function_drafts')) {
      lockVersion = '3'
      functionsDocumentId = documents.state.lastInsertedId || functionsDocumentId
      return { affectedRows: 1, insertId: 0 }
    }
    if (sql.startsWith('INSERT INTO audit_events')) return { insertId: 1, affectedRows: 1 }
    if (sql.includes('FROM edge_function_drafts')) {
      return [draftRow({ lock_version: lockVersion, functions_document_id: functionsDocumentId })]
    }
    return undefined
  })
  const repository = new EdgeFunctionDraftRepository(
    pool,
    documents as never,
    {
      append: async (_executor: unknown, entry: { eventType: string }) => {
        events.push(entry.eventType)
      },
    } as never,
  )

  const view = await repository.save(actor, environmentInternalId, '2', sampleDraft(), 'request-1')

  assert.equal(view.lockVersion, '3')
  assert.deepEqual(view.draft, sampleDraft())
  assert.notEqual(functionsDocumentId, 'doc-1')
  assert.ok(
    documents !== undefined &&
      statements.some((sql) => sql.startsWith('UPDATE edge_function_drafts')),
  )
  assert.deepEqual(events, ['edge_function_draft.updated'])
})

test('getOrCreate returns the existing draft without inserting', async () => {
  const documents = documentsFake(new Map([['doc-1', canonicalJson(emptyEdgeFunctionsDraft())]]))
  const { pool, statements } = buildPool((sql) => {
    if (sql.includes('FROM edge_function_drafts')) return [draftRow()]
    return undefined
  })
  const repository = new EdgeFunctionDraftRepository(pool, documents as never)

  const view = await repository.getOrCreate(actor, environmentInternalId, 'request-1')

  assert.equal(view.id, draftPublicId)
  assert.ok(statements.every((sql) => !sql.startsWith('INSERT INTO edge_function_drafts')))
})
