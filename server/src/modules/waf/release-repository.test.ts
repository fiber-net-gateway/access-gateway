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

interface ScriptResourceFixture {
  name: string
  script: string
  targetOverride?: Buffer
  /** Omit the payload document (simulates an unreadable/missing document). */
  dropDocument?: boolean
}

interface Fixture {
  diffSummary: Record<string, unknown>
  indexText: string | null
  scripts: ScriptResourceFixture[]
  indexTargetOverride?: Buffer
}

function rollbackRow(fixture: Fixture): RowDataPacket {
  return {
    internal_id: '900',
    public_id: publicIdToBuffer(releasePublicId),
    sequence_no: '4',
    compiler_revision: 'waf-generator-v1',
    diff_summary_json: fixture.diffSummary,
    index_document_id: fixture.indexText === null ? null : 'index-doc',
    index_target_sha256:
      fixture.indexText === null
        ? null
        : (fixture.indexTargetOverride ??
          Buffer.from(sha256(fixture.indexText).toString('hex'), 'hex')),
  } as RowDataPacket
}

function scriptResourceRows(fixture: Fixture): RowDataPacket[] {
  return fixture.scripts.map((script, index) => ({
    data_id: `ploto.unified-access.edge-function.${script.name}`,
    payload_document_id: `script-doc-${index}`,
    target_sha256:
      script.targetOverride ?? Buffer.from(sha256(script.script).toString('hex'), 'hex'),
  })) as RowDataPacket[]
}

function buildRepository(fixture: Fixture) {
  const documents = new Map<string, string>([
    ['index-doc', fixture.indexText ?? ''],
    ...fixture.scripts.map((script, index): [string, string] => [
      `script-doc-${index}`,
      script.dropDocument ? '' : script.script,
    ]),
  ])
  const pool = {
    async execute(sql: string) {
      const rows = sql.includes('FROM release_resources')
        ? scriptResourceRows(fixture)
        : [rollbackRow(fixture)]
      return [rows, []] as unknown as [RowDataPacket[], unknown[]]
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
    scripts: [{ name: 'waf', script: scriptText }],
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
      scripts: [{ name: 'waf', script: scriptText }],
      ruleCount: 3,
    },
  } satisfies WafRollbackSource)
})

test('loadRollbackSource rebuilds a multi-function plan with every script', async () => {
  const trapScript =
    'if (($header.user_agent contains "zgrab")) { ctx.report("bot.hit", "zgrab"); }\nreturn true;'
  const trapSha = sha256(trapScript).toString('hex')
  const multiFunctions = [
    { name: 'bot-trap', sha256: trapSha, mounts: [{ projects: { include: ['*'] }, priority: 50 }] },
    ...functions,
  ]
  const { repository } = buildRepository({
    diffSummary: mountedSummary,
    indexText: canonicalJson({ version: 4, functions: multiFunctions }),
    scripts: [
      { name: 'waf', script: scriptText },
      { name: 'bot-trap', script: trapScript },
    ],
  })

  const source = await repository.loadRollbackSource(environmentInternalId, releasePublicId)

  assert.equal(source?.plan.mode, 'mounted')
  assert.deepEqual(
    source?.plan.functions.map((entry) => entry.name),
    ['bot-trap', 'waf'],
  )
  assert.deepEqual(source?.plan.scripts, [
    { name: 'waf', script: scriptText },
    { name: 'bot-trap', script: trapScript },
  ])
  // Publication order follows publish_order, so the rebuilt script list
  // preserves the resource order (waf first in this fixture).
  assert.equal(source?.plan.script, scriptText)
  assert.equal(source?.plan.scriptSha256, scriptSha256)
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
    scripts: [],
  })

  const source = await repository.loadRollbackSource(environmentInternalId, releasePublicId)

  assert.equal(source?.plan.mode, 'unmount')
  assert.equal(source?.plan.script, null)
  assert.deepEqual(source?.plan.functions, [])
  assert.deepEqual(source?.plan.scripts, [])
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
      scripts: [{ name: 'waf', script: scriptText }],
      indexTargetOverride: Buffer.from(sha256(mountedIndexText).toString('hex'), 'hex'),
    },
    'index digest mismatch',
  )
  // Script bytes no longer hash to the release record.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: mountedIndexText,
      scripts: [
        { name: 'waf', script: scriptText, targetOverride: Buffer.from('f'.repeat(64), 'hex') },
      ],
    },
    'script digest mismatch',
  )
  // The stored index references bytes the release does not carry.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: canonicalJson({
        version: 4,
        functions: [{ ...functions[0]!, sha256: 'e'.repeat(64) }],
      }),
      scripts: [{ name: 'waf', script: scriptText }],
    },
    'index does not reference script',
  )
  // A release carrying a script the index does not reference is unusable.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: canonicalJson({ version: 4, functions: [] }),
      scripts: [{ name: 'waf', script: scriptText }],
    },
    'script not referenced by index',
  )
  // An unmount release whose index still carries the WAF function is a
  // contradiction (the release may not publish what its own summary unmounted).
  await expectsInvalid(
    {
      diffSummary: { ...mountedSummary, mode: 'unmount' },
      indexText: mountedIndexText,
      scripts: [{ name: 'waf', script: scriptText }],
    },
    'unmount with waf script',
  )
  // A mounted release without a script document is unusable.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: mountedIndexText,
      scripts: [],
    },
    'mounted without script',
  )
  // A script payload document that cannot be decrypted is unusable.
  await expectsInvalid(
    {
      diffSummary: mountedSummary,
      indexText: mountedIndexText,
      scripts: [{ name: 'waf', script: scriptText, dropDocument: true }],
    },
    'script document unreadable',
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
      scripts: [{ name: 'waf', script: scriptText }],
    },
    'index not a function array',
  )
  await expectsInvalid(
    {
      diffSummary: { ...mountedSummary, mode: 'mystery' },
      indexText: mountedIndexText,
      scripts: [{ name: 'waf', script: scriptText }],
    },
    'unknown diff mode',
  )
  await expectsInvalid(
    {
      diffSummary: { ...mountedSummary, ruleCount: -1 },
      indexText: mountedIndexText,
      scripts: [{ name: 'waf', script: scriptText }],
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
