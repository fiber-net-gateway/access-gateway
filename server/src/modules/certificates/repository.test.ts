import assert from 'node:assert/strict'
import test from 'node:test'

import type { PoolConnection } from 'mysql2/promise'

import type { DocumentCipher, EncryptedDocument } from '../../crypto/document-cipher.js'
import { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import type { Actor } from '../auth/model.js'
import type { ParsedCertificateUpload } from './parser.js'
import { CertificateRepository, certificateSniCoverageChanges } from './repository.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}

const encryptedDocument: EncryptedDocument = {
  plaintextSha256: Buffer.alloc(32),
  plaintextSize: 1,
  keyId: 'test',
  wrappedDek: Buffer.alloc(60),
  nonce: Buffer.alloc(12),
  authTag: Buffer.alloc(16),
  ciphertext: Buffer.alloc(1),
}

const documents = new DocumentRepository({
  encrypt: () => encryptedDocument,
  decrypt: () => Buffer.alloc(0),
} satisfies DocumentCipher)

const parsedUpload: ParsedCertificateUpload = {
  name: 'API certificate',
  certificatePem: 'certificate',
  privateKeyPem: 'private key',
  fingerprintSha256: Buffer.alloc(32, 1),
  serialNumber: '01',
  subject: 'CN=new.example.com',
  issuer: 'CN=Test CA',
  dnsNames: ['new.example.com'],
  notBefore: new Date('2026-01-01T00:00:00.000Z'),
  notAfter: new Date('2027-01-01T00:00:00.000Z'),
  keyType: 'ec',
}

test('computes automatic SNI coverage changes from certificate DNS SAN sets', () => {
  assert.deepEqual(
    certificateSniCoverageChanges(
      ['api.example.com', '*.internal.example.com'],
      ['*.internal.example.com', 'new.example.com'],
    ),
    {
      added: ['new.example.com'],
      removed: ['api.example.com'],
    },
  )
})

test('treats DNS SAN order changes as the same automatic SNI coverage', () => {
  assert.deepEqual(
    certificateSniCoverageChanges(
      ['api.example.com', '*.internal.example.com'],
      ['*.internal.example.com', 'api.example.com'],
    ),
    { added: [], removed: [] },
  )
})

test('rolls back before storage writes when SAN coverage changes without confirmation', async () => {
  const statements: string[] = []
  let committed = false
  let rolledBack = false
  let released = false
  const connection = {
    query: async () => [[], []],
    beginTransaction: async () => undefined,
    commit: async () => {
      committed = true
    },
    rollback: async () => {
      rolledBack = true
    },
    release: () => {
      released = true
    },
    execute: async (sql: string) => {
      statements.push(sql)
      if (sql.includes('SELECT id FROM environments')) return [[{ id: '3' }], []]
      if (sql.includes('current_version.dns_names_json')) {
        return [
          [
            {
              current_version_id: '11',
              current_version_no: 2,
              current_dns_names_json: '["old.example.com"]',
              lock_version: '4',
            },
          ],
          [],
        ]
      }
      throw new Error(`Unexpected SQL after confirmation gate: ${sql}`)
    },
  } as unknown as PoolConnection
  const pool = {
    getConnection: async () => connection,
  } as unknown as DatabasePool
  const repository = new CertificateRepository(pool, documents)

  await assert.rejects(
    repository.createVersion(
      actor,
      '7',
      '3',
      '00000000-0000-4000-8000-000000000007',
      'API certificate',
      '4',
      parsedUpload,
      false,
      'request-1',
    ),
    (error: unknown) => {
      assert.equal(
        (error as { code?: string }).code,
        'CERTIFICATE_SNI_COVERAGE_CONFIRMATION_REQUIRED',
      )
      assert.deepEqual(
        (error as { fields?: readonly { code: string }[] }).fields?.map((field) => field.code),
        ['SNI_NAME_ADDED', 'SNI_NAME_REMOVED'],
      )
      return true
    },
  )

  assert.equal(statements.length, 2)
  assert.equal(committed, false)
  assert.equal(rolledBack, true)
  assert.equal(released, true)
})
