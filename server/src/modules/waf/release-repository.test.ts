import assert from 'node:assert/strict'
import test from 'node:test'

import type { RowDataPacket } from 'mysql2/promise'

import type { DatabasePool } from '../../database/types.js'
import { AppError } from '../../shared/errors.js'
import { publicIdToBuffer } from '../../shared/ids.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import { WafReleaseRepository, type WafRollbackSource } from './release-repository.js'

const releasePublicId = '00000000-0000-4000-8000-00000000000c'
const environmentInternalId = '11'

const scriptText = '-- historical waf script bytes'
const scriptSha256 = sha256(scriptText).toString('hex')
const functions = [
  {
    name: 'waf',
    sha256: scriptSha256,
    mounts: [{ projects: { include: ['*'], exclude: ['loan-admin-legacy'] }, priority: 100 }],
  },
]
const mountedIndexText = canonicalJson({ version: 4, functions })

interface Fixture {
  diffSummary: Record<string, unknown>
  indexText: string | null
  scriptText: string | null
  indexTargetOverride?: Buffer
  scriptTargetOverride?: Buffer
}

function rollbackRow(fixture: Fixture): RowDataPacket {
  return {
    internal_id: '900',
    public_id: publicIdToBuffer(releasePublicId),
    sequence_no: '4',
    compiler_revision: 'waf-generator-v1',
    diff_summary_json: JSON.stringify(fixture.diffSummary),
    script_document_id: fixture.scriptText === null ? null : 'script-doc',
    script_target_sha256:
      fixture.scriptText === null
        ? null
        : (fixture.scriptTargetOverride ?? Buffer.from(scriptSha256, 'hex')),
    index_document_id: fixture.indexText === null ? null : 'index-doc',
    index_target_sha256:
      fixture.indexText === null
        ? null
        : (fixture.indexTargetOverride ??
          Buffer.from(sha256(fixture.indexText).toString('hex'), 'hex')),
  } as RowDataPacket
}

function buildRepository(fixture: Fixture) {
  const documents = new Map<string, string>([
    ['index-doc', fixture.indexText ?? ''],
    ['script-doc', fixture.scriptText ?? ''],
  ])
  const pool = {
    async execute() {
      return [
        [
          {
            ...rollbackRow(fixture),
          },
        ],
        [],
      ] as unknown as [RowDataPacket[], unknown[]]
    },
  } as unknown as DatabasePool
  const documentRepository = {
    decryptByInternalId: async (_executor: unknown, internalId: string) => {
      const text = documents.get(internalId)
      return text === undefined ? null : Buffer.from(text, 'utf8')
    },
  }
  const repository = new WafReleaseRepository(pool, documentRepository as never, undefined, null)
  return { repository, documents }
}

const mountedSummary = {
  functionName: 'waf',
  mode: 'mounted',
  scriptSha256,
  ruleCount: 3,
  indexVersion: '4',
}

test('loadRollbackSource rebuilds a mounted plan from verified payload documents', async () => {
  const { repository } = buildRepository({
    diffSummary: mountedSummary,
    indexText: mountedIndexText,
    scriptText,
  })

  const source = await repository.loadRollbackSource(environmentInternalId, releasePublicId)

  assert.deepEqual(source, {
    publicId: releasePublicId,
    sequence: '4',
    generatorRevision: 'waf-generator-v1',
    plan: {
      mode: 'mounted',
      functionName: 'waf',
      script: scriptText,
      scriptSha256,
      functions,
      ruleCount: 3,
    },
  } satisfies WafRollbackSource)
})

test('loadRollbackSource rebuilds an unmount plan with empty functions', async () => {
  const { repository } = buildRepository({
    diffSummary: {
      functionName: 'waf',
      mode: 'unmount',
      scriptSha256: null,
      ruleCount: 0,
      indexVersion: '2',
    },
    indexText: canonicalJson({ version: 2, functions: [] }),
    scriptText: null,
  })

  const source = await repository.loadRollbackSource(environmentInternalId, releasePublicId)

  assert.equal(source?.plan.mode, 'unmount')
  assert.equal(source?.plan.script, null)
  assert.deepEqual(source?.plan.functions, [])
})

test('loadRollbackSource rejects tampered payload documents', async () => {
  const expectsInvalid = async (fixture: Fixture, detail: string) => {
    const { repository } = buildRepository(fixture)
    await assert.rejects(
      repository.loadRollbackSource(environmentInternalId, releasePublicId),
      (error: unknown) => {
        assert.ok(error instanceof AppError, detail)
        assert.equal(error.code, 'WAF_ROLLBACK_PAYLOAD_INVALID')
        assert.equal(error.statusCode, 422)
        return true
      },
    )
  }

  // Index bytes no longer hash to the digest recorded at release time.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: canonicalJson({ version: 4, functions: [] }),
      scriptText,
      indexTargetOverride: Buffer.from(sha256(mountedIndexText).toString('hex'), 'hex'),
    },
    'index digest mismatch',
  )
  // Script bytes no longer hash to the release record.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: mountedIndexText,
      scriptText: '-- tampered script',
    },
    'script digest mismatch',
  )
  // The stored index does not reference the script bytes.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: canonicalJson({
        version: 4,
        functions: [{ ...functions[0]!, sha256: 'e'.repeat(64) }],
      }),
      scriptText,
    },
    'index does not reference script',
  )
  // An unmount release must not carry mounted functions.
  await expectsInvalid(
    {
      diffSummary: { ...mountedSummary, mode: 'unmount' },
      indexText: mountedIndexText,
      scriptText: null,
    },
    'unmount with functions',
  )
  // A mounted release without a script document is unusable.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: mountedIndexText,
      scriptText: null,
    },
    'mounted without script',
  )
})

test('loadRollbackSource rejects malformed stored payloads', async () => {
  const expectsInvalid = async (fixture: Fixture, detail: string) => {
    const { repository } = buildRepository(fixture)
    await assert.rejects(
      repository.loadRollbackSource(environmentInternalId, releasePublicId),
      (error: unknown) => {
        assert.ok(error instanceof AppError, detail)
        assert.equal(error.code, 'WAF_ROLLBACK_PAYLOAD_INVALID')
        return true
      },
    )
  }

  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: '{"version":4,"functions":"nope"}',
      scriptText,
    },
    'index not a function array',
  )
  await expectsInvalid(
    {
      diffSummary: { ...mountedSummary, mode: 'mystery' },
      indexText: mountedIndexText,
      scriptText,
    },
    'unknown diff mode',
  )
  await expectsInvalid(
    {
      diffSummary: { ...mountedSummary, ruleCount: -1 },
      indexText: mountedIndexText,
      scriptText,
    },
    'invalid rule count',
  )
})

test('loadRollbackSource returns null for an unknown release', async () => {
  const pool = {
    async execute() {
      return [[]] as unknown as [RowDataPacket[], unknown[]]
    },
  } as unknown as DatabasePool
  const repository = new WafReleaseRepository(pool, {
    decryptByInternalId: async () => null,
  } as never)

  assert.equal(await repository.loadRollbackSource(environmentInternalId, releasePublicId), null)
})
