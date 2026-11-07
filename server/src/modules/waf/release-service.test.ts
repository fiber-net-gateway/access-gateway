import assert from 'node:assert/strict'
import test from 'node:test'

import type {
  NacosClient,
  NacosResourceValue,
  NacosTarget,
} from '../../integrations/nacos/model.js'
import type {
  NativeValidationRequest,
  NativeValidationResult,
  NativeValidator,
} from '../../integrations/native-validator/model.js'
import { AppError } from '../../shared/errors.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { WafPolicy } from './model.js'
import {
  EDGE_FUNCTIONS_DATA_ID,
  EDGE_FUNCTIONS_GROUP,
  edgeFunctionScriptDataId,
  type WafReleaseView,
} from './release-model.js'
import type { PreparedWafReleasePlan, WafRollbackSource } from './release-repository.js'
import { WafReleaseRepository } from './release-repository.js'
import {
  DefaultWafReleaseService,
  UnavailableWafReleaseService,
  WAF_GENERATOR_REVISION,
} from './release-service.js'
import { WafPolicyDraftRepository, type WafPolicyDraftView } from './repository.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Publisher',
  platformAdmin: false,
}

const environmentPublicId = '00000000-0000-4000-8000-00000000000a'

const environmentView = {
  id: environmentPublicId,
  code: 'prod',
  name: 'Production',
  tier: 'production',
  status: 'active',
  nacos: {
    endpoint: 'http://nacos:8848',
    namespace: 'ns',
    tenant: 'tenant',
    credentialConfigured: false,
  },
  dataIds: {},
  zone: 'zone-1',
  protectionPolicy: {},
  lockVersion: '0',
  createdAt: '2026-01-01T00:00:00.000Z',
  updatedAt: '2026-01-01T00:00:00.000Z',
}

function environmentsFake(role: string | null = 'publisher') {
  return {
    findWorkspace: async () => environmentView,
    internalIdForActor: async () => '11',
    role: async () => role,
  } as unknown as EnvironmentRepository
}

function validatorFake(overrides: Partial<NativeValidationResult> = {}) {
  const requests: NativeValidationRequest[] = []
  const validator = {
    available: true,
    contractVersion: 1,
    revision: 'deadbeef',
    limits: { schemaVersion: 4 },
    validate: async (request: NativeValidationRequest): Promise<NativeValidationResult> => {
      requests.push(request)
      return {
        valid: true,
        errors: [],
        contractVersion: 1,
        validatorRevision: 'deadbeef',
        ...overrides,
      }
    },
  }
  return { validator: validator as unknown as NativeValidator, requests }
}

function nacosFake(exists = false) {
  const reads: { dataId: string; group: string }[] = []
  const value: NacosResourceValue = {
    exists,
    content: exists ? '{}' : null,
    sha256: exists ? '00'.repeat(32) : null,
    md5: exists ? '00'.repeat(16) : null,
  }
  const nacos = {
    available: true,
    read: async (_target: NacosTarget, dataId: string, group: string) => {
      reads.push({ dataId, group })
      return value
    },
    write: async () => {
      throw new Error('Unexpected nacos write during release preparation')
    },
  }
  return { nacos: nacos as unknown as NacosClient, reads }
}

const draftView = (policy: WafPolicy): WafPolicyDraftView => ({
  id: '00000000-0000-4000-8000-00000000000b',
  policy,
  policySha256: '0'.repeat(64),
  lockVersion: '2',
  createdAt: '2026-11-06T00:00:00.000Z',
  updatedAt: '2026-11-06T00:00:00.000Z',
})

function draftsFake(policy: WafPolicy | null) {
  return {
    findByEnvironmentInternalId: async () => (policy === null ? null : draftView(policy)),
  } as unknown as WafPolicyDraftRepository
}

const releaseView: WafReleaseView = {
  id: '00000000-0000-4000-8000-00000000000c',
  sequence: '5',
  status: 'ready',
  functionName: 'waf',
  mode: 'mounted',
  scriptSha256: '1'.repeat(64),
  ruleCount: 1,
  indexVersion: '5',
  description: '',
  resources: [],
  publication: { jobId: null, state: null },
  activationStatus: 'unknown',
  activation: {
    status: 'unknown',
    targetCount: 0,
    activeCount: 0,
    pendingCount: 0,
    degradedCount: 0,
    unknownCount: 0,
    evaluatedAt: null,
  },
  createdAt: '2026-11-07T00:00:00.000Z',
  publishedAt: null,
}

function releasesFake() {
  const created: {
    environmentInternalId: string
    plan: PreparedWafReleasePlan
    bases: { index: NacosResourceValue; script: NacosResourceValue | null }
    input: Record<string, unknown>
  }[] = []
  const repository = {
    create: async (
      actorArg: unknown,
      environmentInternalId: string,
      plan: PreparedWafReleasePlan,
      bases: { index: NacosResourceValue; script: NacosResourceValue | null },
      input: Record<string, unknown>,
    ) => {
      assert.equal(actorArg, actor)
      created.push({ environmentInternalId, plan, bases, input })
      return releaseView
    },
    list: async () => [releaseView],
    find: async () => releaseView,
    queuePublication: async () => ({
      jobId: '00000000-0000-4000-8000-00000000000d',
      state: 'queued',
      release: releaseView,
    }),
  } as unknown as WafReleaseRepository
  return { repository, created }
}

function mountedPolicy(): WafPolicy {
  return {
    schemaVersion: 1,
    defaultMode: 'log',
    rules: [
      {
        id: 'probe-0001',
        name: 'probe',
        severity: 'high',
        priority: 100,
        action: 'block',
        phase: 'request_head',
        match: 'strings.contains($req.path, "../")',
      },
    ],
    projectModes: { 'loan-admin-legacy': 'off' },
  }
}

function buildService(overrides: {
  policy?: WafPolicy | null
  role?: string | null
  validatorOverrides?: Partial<NativeValidationResult>
  validatorAvailable?: boolean
  nacosAvailable?: boolean
  drafts?: WafPolicyDraftRepository
  releases?: WafReleaseRepository
}) {
  const { validator, requests } = validatorFake(overrides.validatorOverrides)
  if (overrides.validatorAvailable === false) {
    ;(validator as { available: boolean }).available = false
  }
  const { nacos, reads } = nacosFake()
  if (overrides.nacosAvailable === false) {
    ;(nacos as { available: boolean }).available = false
  }
  const { repository, created } = overrides.releases
    ? { repository: overrides.releases, created: null }
    : releasesFake()
  const subject = new DefaultWafReleaseService(
    overrides.drafts ??
      draftsFake(overrides.policy === undefined ? mountedPolicy() : overrides.policy),
    repository,
    environmentsFake(overrides.role),
    validator,
    nacos,
  )
  return { service: subject, requests, reads, created }
}

test('create compiles a mounted policy into script + index payloads and reads both bases', async () => {
  const { service, requests, reads, created } = buildService({})
  const release = await service.create(actor, { idempotencyKey: 'key-1' }, 'request-1')
  assert.equal(release, releaseView)

  const scriptRequest = requests.find((request) => request.kind === 'edge_function_script')
  const indexRequest = requests.find((request) => request.kind === 'edge_function_index')
  assert.ok(scriptRequest)
  assert.ok(indexRequest)
  assert.equal(requests.length, 2)

  const plan = created?.[0]?.plan
  assert.ok(plan)
  assert.equal(plan.mode, 'mounted')
  assert.equal(plan.functionName, 'waf')
  assert.equal(plan.ruleCount, 1)
  assert.ok(plan.script)
  // The validator saw the exact script bytes that become the script payload.
  assert.equal(Buffer.from(scriptRequest.payload).toString('utf8'), plan.script)
  // The validated index is structurally identical to the published one; only
  // the version integer differs (repository embeds the allocated sequence).
  const validatedIndex = JSON.parse(Buffer.from(indexRequest.payload).toString('utf8')) as {
    version: number
    functions: { name: string; sha256: string }[]
  }
  assert.equal(validatedIndex.version, 1)
  assert.deepEqual(validatedIndex.functions, [
    {
      name: 'waf',
      sha256: plan.scriptSha256,
      mounts: [{ projects: { include: ['*'], exclude: ['loan-admin-legacy'] }, priority: 100 }],
    },
  ])
  assert.deepEqual(plan.functions, validatedIndex.functions)

  assert.deepEqual(reads, [
    { dataId: EDGE_FUNCTIONS_DATA_ID, group: EDGE_FUNCTIONS_GROUP },
    { dataId: edgeFunctionScriptDataId('waf'), group: EDGE_FUNCTIONS_GROUP },
  ])
  const bases = created?.[0]?.bases
  assert.ok(bases?.index)
  assert.ok(bases?.script)

  const input = created?.[0]?.input
  assert.equal(input?.generatorRevision, WAF_GENERATOR_REVISION)
  assert.equal(input?.validatorRevision, 'deadbeef')
  assert.equal(input?.idempotencyKey, 'key-1')
})

test('create compiles an empty policy into an unmount release with only an index resource', async () => {
  const { service, requests, reads, created } = buildService({
    policy: { schemaVersion: 1, defaultMode: 'off', rules: [], projectModes: {} },
  })
  await service.create(actor, { idempotencyKey: 'key-2' }, 'request-2')

  assert.equal(requests.length, 1)
  assert.equal(requests[0]?.kind, 'edge_function_index')
  const validatedIndex = JSON.parse(Buffer.from(requests[0]?.payload ?? '').toString('utf8')) as {
    functions: unknown[]
  }
  assert.deepEqual(validatedIndex.functions, [])

  assert.deepEqual(reads, [{ dataId: EDGE_FUNCTIONS_DATA_ID, group: EDGE_FUNCTIONS_GROUP }])
  const plan = created?.[0]?.plan
  assert.equal(plan?.mode, 'unmount')
  assert.equal(plan?.script, null)
  assert.equal(created?.[0]?.bases.script, null)
})

test('create fails closed when the compiled script does not validate', async () => {
  let persisted = false
  const { repository } = releasesFake()
  ;(repository as unknown as { create: () => never }).create = () => {
    persisted = true
    throw new Error('Unexpected persistence')
  }
  const { service } = buildService({
    validatorOverrides: {
      valid: false,
      errors: [{ code: 'script_compile_failed', offset: 12, message: 'unexpected escape' }],
    },
    releases: repository,
  })
  await assert.rejects(
    service.create(actor, { idempotencyKey: 'key-3' }, 'request-3'),
    (error: unknown) => {
      assert.ok(error instanceof AppError)
      assert.equal(error.code, 'WAF_COMPILATION_NOT_PUBLISHABLE')
      assert.equal(error.statusCode, 422)
      return true
    },
  )
  assert.equal(persisted, false)
})

test('create requires a usable native validator, nacos, and an existing draft', async () => {
  const expectStatus = async (
    build: () => { service: DefaultWafReleaseService },
    statusCode: number,
  ) => {
    const { service: subject } = build()
    await assert.rejects(
      subject.create(actor, { idempotencyKey: 'k' }, 'r'),
      (error: unknown) => error instanceof AppError && error.statusCode === statusCode,
    )
  }
  await expectStatus(() => buildService({ validatorAvailable: false }), 503)
  await expectStatus(() => buildService({ nacosAvailable: false }), 503)
  await expectStatus(() => buildService({ policy: null }), 404)
})

test('create requires the publisher role', async () => {
  const { service } = buildService({ role: 'viewer' })
  await assert.rejects(
    service.create(actor, { idempotencyKey: 'k' }, 'r'),
    (error: unknown) => error instanceof AppError && error.statusCode === 403,
  )
})

test('queuePublication resolves the release and queues through the repository', async () => {
  const queued: string[] = []
  const repository = {
    find: async () => releaseView,
    queuePublication: async (_actor: unknown, releaseId: string) => {
      queued.push(releaseId)
      return { jobId: 'job-1', state: 'queued', release: releaseView }
    },
  } as unknown as WafReleaseRepository
  const { service } = buildService({ releases: repository })
  const result = await service.queuePublication(actor, releaseView.id, 'idem', 'request-4')
  assert.equal(result.jobId, 'job-1')
  assert.deepEqual(queued, [releaseView.id])
})

test('UnavailableWafReleaseService rejects every operation as unavailable', async () => {
  const subject = new UnavailableWafReleaseService()
  const expectsUnavailable = (error: unknown): boolean =>
    error instanceof AppError && error.statusCode === 503
  await assert.rejects(subject.create(), expectsUnavailable)
  await assert.rejects(subject.list(), expectsUnavailable)
  await assert.rejects(subject.get(), expectsUnavailable)
  await assert.rejects(subject.queuePublication(), expectsUnavailable)
})

// The representative index the service validates must serialize exactly like
// the published one apart from the version integer, so canonical key ordering
// and nested shapes stay identical.
test('representative index serialization is stable under canonicalJson', () => {
  const functions = [
    {
      name: 'waf',
      sha256: 'a'.repeat(64),
      mounts: [{ projects: { include: ['*'], exclude: [] }, priority: 100 }],
    },
  ]
  assert.equal(
    canonicalJson({ version: 1, functions }),
    `{"functions":[{"mounts":[{"priority":100,"projects":{"exclude":[],"include":["*"]}}],"name":"waf","sha256":"${'a'.repeat(64)}"}],"version":1}`,
  )
})

const historicalScript = '-- historical waf script bytes'

function rollbackSourceMounted(): WafRollbackSource {
  return {
    publicId: '00000000-0000-4000-8000-00000000000e',
    sequence: '4',
    generatorRevision: 'waf-generator-v0-old',
    plan: {
      mode: 'mounted',
      functionName: 'waf',
      script: historicalScript,
      scriptSha256: sha256(historicalScript).toString('hex'),
      functions: [
        {
          name: 'waf',
          sha256: sha256(historicalScript).toString('hex'),
          mounts: [{ projects: { include: ['*'], exclude: [] }, priority: 100 }],
        },
      ],
      ruleCount: 3,
    },
  }
}

interface CreatedCapture {
  environmentInternalId: string
  plan: PreparedWafReleasePlan
  bases: { index: NacosResourceValue; script: NacosResourceValue | null }
  input: Record<string, unknown>
}

function rollbackRepositoryFake(source: WafRollbackSource | null) {
  const created: CreatedCapture[] = []
  const repository = {
    loadRollbackSource: async (_environmentInternalId: string, releaseId: string) =>
      source && releaseId === source.publicId ? source : null,
    create: async (
      _actor: unknown,
      environmentInternalId: string,
      plan: PreparedWafReleasePlan,
      bases: { index: NacosResourceValue; script: NacosResourceValue | null },
      input: Record<string, unknown>,
    ) => {
      created.push({ environmentInternalId, plan, bases, input })
      return releaseView
    },
  }
  return { repository: repository as unknown as WafReleaseRepository, created }
}

test('rollback republishes historical payloads through the same fail-closed pipeline', async () => {
  const source = rollbackSourceMounted()
  const { repository, created } = rollbackRepositoryFake(source)
  const { service, requests, reads } = buildService({ releases: repository })

  const result = await service.rollback(actor, source.publicId, { idempotencyKey: 'rb-1' }, 'rb')
  assert.equal(result, releaseView)

  // The validator saw the exact historical script bytes and a representative
  // index carrying the historical functions (only the version integer differs).
  const scriptRequest = requests.find((request) => request.kind === 'edge_function_script')
  const indexRequest = requests.find((request) => request.kind === 'edge_function_index')
  assert.ok(scriptRequest)
  assert.ok(indexRequest)
  assert.equal(Buffer.from(scriptRequest.payload).toString('utf8'), historicalScript)
  const validatedIndex = JSON.parse(Buffer.from(indexRequest.payload).toString('utf8')) as {
    version: number
    functions: unknown[]
  }
  assert.equal(validatedIndex.version, 1)
  assert.deepEqual(validatedIndex.functions, source.plan.functions)

  assert.deepEqual(reads, [
    { dataId: EDGE_FUNCTIONS_DATA_ID, group: EDGE_FUNCTIONS_GROUP },
    { dataId: edgeFunctionScriptDataId('waf'), group: EDGE_FUNCTIONS_GROUP },
  ])

  assert.equal(created.length, 1)
  assert.equal(created[0]?.plan, source.plan)
  const input = created[0]?.input
  assert.equal(input?.idempotencyKey, 'rb-1')
  // Provenance: the content's historical generator revision, not today's.
  assert.equal(input?.generatorRevision, 'waf-generator-v0-old')
  assert.equal(input?.rollbackOfSequence, '4')
  assert.equal(input?.description, 'rollback of WAF edge functions #4')
  // The rollback request identity differs from a direct create, so replaying
  // the idempotency key against a create request is rejected.
  assert.notEqual(
    (input?.requestSha256 as Buffer | undefined)?.toString('hex'),
    sha256(canonicalJson({ kind: 'waf_edge_functions' })).toString('hex'),
  )
})

test('rollback of an unmount source validates and reads only the index', async () => {
  const source: WafRollbackSource = {
    publicId: '00000000-0000-4000-8000-00000000000f',
    sequence: '2',
    generatorRevision: 'waf-generator-v1',
    plan: {
      mode: 'unmount',
      functionName: 'waf',
      script: null,
      scriptSha256: null,
      functions: [],
      ruleCount: 0,
    },
  }
  const { repository, created } = rollbackRepositoryFake(source)
  const { service, requests, reads } = buildService({ releases: repository })

  await service.rollback(actor, source.publicId, { idempotencyKey: 'rb-2' }, 'rb')

  assert.equal(requests.length, 1)
  assert.equal(requests[0]?.kind, 'edge_function_index')
  assert.deepEqual(reads, [{ dataId: EDGE_FUNCTIONS_DATA_ID, group: EDGE_FUNCTIONS_GROUP }])
  assert.equal(created[0]?.plan.mode, 'unmount')
  assert.equal(created[0]?.bases.script, null)
})

test('rollback fails closed on stale payloads and unknown releases', async () => {
  let persisted = false
  const source = rollbackSourceMounted()
  const { repository } = rollbackRepositoryFake(source)
  ;(repository as unknown as { create: () => never }).create = () => {
    persisted = true
    throw new Error('Unexpected persistence')
  }
  const { service } = buildService({
    validatorOverrides: {
      valid: false,
      errors: [{ code: 'script_compile_failed', offset: 3, message: 'stale script' }],
    },
    releases: repository,
  })
  await assert.rejects(
    service.rollback(actor, source.publicId, { idempotencyKey: 'rb-3' }, 'rb'),
    (error: unknown) => {
      assert.ok(error instanceof AppError)
      assert.equal(error.code, 'WAF_ROLLBACK_NOT_PUBLISHABLE')
      assert.equal(error.statusCode, 422)
      return true
    },
  )
  assert.equal(persisted, false)

  const missing = rollbackRepositoryFake(null)
  const { service: empty } = buildService({ releases: missing.repository })
  await assert.rejects(
    empty.rollback(actor, '00000000-0000-4000-8000-0000000000ff', { idempotencyKey: 'k' }, 'r'),
    (error: unknown) => error instanceof AppError && error.statusCode === 404,
  )
})

test('rollback requires the publisher role and rejects rollbacks while unavailable', async () => {
  const source = rollbackSourceMounted()
  const { repository } = rollbackRepositoryFake(source)
  const { service } = buildService({ role: 'viewer', releases: repository })
  await assert.rejects(
    service.rollback(actor, source.publicId, { idempotencyKey: 'k' }, 'r'),
    (error: unknown) => error instanceof AppError && error.statusCode === 403,
  )

  const unavailableService = new UnavailableWafReleaseService()
  await assert.rejects(
    unavailableService.rollback(),
    (error: unknown) => error instanceof AppError && error.statusCode === 503,
  )
})
