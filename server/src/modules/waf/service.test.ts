import assert from 'node:assert/strict'
import test from 'node:test'

import type {
  NativeValidationRequest,
  NativeValidationResult,
  NativeValidator,
} from '../../integrations/native-validator/model.js'
import { AppError } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { WafPolicyDraftView } from './repository.js'
import { WafPolicyDraftRepository } from './repository.js'
import { compileWafPolicy } from './generator.js'
import { DefaultWafService, UnavailableWafService } from './service.js'
import type { WafPolicy } from './model.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}

const environmentPublicId = '00000000-0000-4000-8000-00000000000a'
const environmentInternalId = '11'

function environmentsFake(role: string | null = 'maintainer') {
  return {
    findWorkspace: async () => ({ id: environmentPublicId, name: 'workspace' }),
    internalIdForActor: async () => environmentInternalId,
    role: async () => role,
  } as unknown as EnvironmentRepository
}

function validatorFake(overrides: Partial<NativeValidationResult> = {}) {
  const requests: NativeValidationRequest[] = []
  const validator = {
    available: true,
    contractVersion: 1,
    revision: 'deadbeef',
    limits: null,
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

function policy(overrides: Partial<WafPolicy> = {}): WafPolicy {
  return {
    schemaVersion: 2,
    groups: [
      { id: 'core', name: 'core', mode: 'log', priority: 100, projects: { include: ['*'] } },
    ],
    rules: [
      {
        id: 'probe-0001',
        name: 'probe',
        group: 'core',
        severity: 'high',
        priority: 100,
        action: 'block',
        phase: 'request_head',
        match: 'strings.contains($req.path, "../")',
      },
    ],
    projectModes: {},
    ...overrides,
  }
}

const draftView: WafPolicyDraftView = {
  id: '00000000-0000-4000-8000-00000000000b',
  policy: policy(),
  policySha256: '0'.repeat(64),
  lockVersion: '1',
  createdAt: '2026-11-06T00:00:00.000Z',
  updatedAt: '2026-11-06T00:00:00.000Z',
}

test('savePolicyDraft rejects an invalid policy before persistence', async () => {
  let persisted = false
  const drafts = {
    save: async () => {
      persisted = true
      throw new Error('Unexpected persistence')
    },
  } as unknown as WafPolicyDraftRepository
  const service = new DefaultWafService(drafts, environmentsFake(), validatorFake().validator)

  const invalid = policy({
    rules: [policy().rules[0]!, { ...policy().rules[0]!, name: 'duplicate id' }],
  })
  await assert.rejects(
    service.savePolicyDraft(actor, { lockVersion: '0', policy: invalid }, 'request-1'),
    (error: unknown) => {
      assert.ok(error instanceof AppError)
      assert.equal(error.code, 'INVALID_WAF_POLICY')
      assert.ok(
        error.fields.some((field) => field.path === 'rules[1].id' && field.code === 'conflict'),
      )
      return true
    },
  )
  assert.equal(persisted, false)
})

test('savePolicyDraft rejects a malformed lock version before persistence', async () => {
  let persisted = false
  const drafts = {
    save: async () => {
      persisted = true
      throw new Error('Unexpected persistence')
    },
  } as unknown as WafPolicyDraftRepository
  const service = new DefaultWafService(drafts, environmentsFake(), validatorFake().validator)

  await assert.rejects(
    service.savePolicyDraft(actor, { lockVersion: 'x', policy: policy() }, 'request-1'),
    (error: unknown) => error instanceof AppError && error.code === 'INVALID_LOCK_VERSION',
  )
  assert.equal(persisted, false)
})

test('savePolicyDraft persists a validated policy with the parsed lock version', async () => {
  const saved: unknown[] = []
  const drafts = {
    save: async (
      actorArg: unknown,
      environmentInternalId: string,
      expectedLockVersion: string,
      savedPolicy: WafPolicy,
    ) => {
      saved.push({ actorArg, environmentInternalId, expectedLockVersion, savedPolicy })
      return draftView
    },
  } as unknown as WafPolicyDraftRepository
  const service = new DefaultWafService(drafts, environmentsFake(), validatorFake().validator)

  const result = await service.savePolicyDraft(actor, { lockVersion: '3', policy: policy() }, 'r')
  assert.equal(result, draftView)
  assert.deepEqual(saved, [
    {
      actorArg: actor,
      environmentInternalId,
      expectedLockVersion: '3',
      savedPolicy: policy(),
    },
  ])
})

test('savePolicyDraft requires an editor role', async () => {
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake('viewer'),
    validatorFake().validator,
  )
  await assert.rejects(
    service.savePolicyDraft(actor, { lockVersion: '0', policy: policy() }, 'request-1'),
    (error: unknown) => error instanceof AppError && error.statusCode === 403,
  )
})

test('getPolicyDraft reports 404 when no draft exists', async () => {
  const drafts = {
    findByEnvironmentInternalId: async () => null,
  } as unknown as WafPolicyDraftRepository
  const service = new DefaultWafService(drafts, environmentsFake(), validatorFake().validator)

  await assert.rejects(
    service.getPolicyDraft(actor),
    (error: unknown) => error instanceof AppError && error.statusCode === 404,
  )
})

test('getPolicyDraft returns the stored draft view', async () => {
  const drafts = {
    findByEnvironmentInternalId: async () => draftView,
  } as unknown as WafPolicyDraftRepository
  const service = new DefaultWafService(drafts, environmentsFake(), validatorFake().validator)

  assert.equal(await service.getPolicyDraft(actor), draftView)
})

test('createPolicyDraft delegates to getOrCreate on the workspace', async () => {
  const calls: unknown[] = []
  const drafts = {
    getOrCreate: async (actorArg: unknown, environmentInternalId: string, requestId: string) => {
      calls.push({ actorArg, environmentInternalId, requestId })
      return draftView
    },
  } as unknown as WafPolicyDraftRepository
  const service = new DefaultWafService(drafts, environmentsFake(), validatorFake().validator)

  assert.equal(await service.createPolicyDraft(actor, 'request-9'), draftView)
  assert.deepEqual(calls, [{ actorArg: actor, environmentInternalId, requestId: 'request-9' }])
})

test('compilePreview reports field issues without compiling or invoking the validator', async () => {
  const { validator, requests } = validatorFake()
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake(),
    validator,
  )

  const result = await service.compilePreview(
    actor,
    policy({
      rules: [policy().rules[0]!, { ...policy().rules[0]!, name: 'duplicate id' }],
    }),
    'request-1',
  )
  assert.equal(result.valid, false)
  assert.ok(
    result.issues.some((issue) => issue.field === 'rules[1].id' && issue.code === 'conflict'),
  )
  assert.equal(result.compilation, null)
  assert.equal(result.scriptValid, null)
  assert.deepEqual(requests, [])
})

test('compilePreview compiles a valid policy through the native validator', async () => {
  const { validator, requests } = validatorFake()
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake(),
    validator,
  )

  const result = await service.compilePreview(actor, policy(), 'request-1')
  assert.equal(result.valid, true)
  assert.deepEqual(result.issues, [])
  assert.equal(result.scriptValid, true)
  assert.deepEqual(result.scriptErrors, [])
  assert.deepEqual(result.validator, { contractVersion: 1, revision: 'deadbeef' })
  assert.equal(result.compilation?.kind, 'mounted')
  assert.equal(requests.length, 1)
  const request = requests[0]!
  assert.equal(request.kind, 'edge_function_script')
  assert.equal(request.project, '')
  if (result.compilation?.kind === 'mounted') {
    assert.equal(
      Buffer.from(request.payload).toString('utf8'),
      result.compilation.functions[0]?.script,
    )
  }
})

test('compilePreview compiles every group and pins errors to their own function', async () => {
  const input = policy({
    groups: [...policy().groups, { ...policy().groups[0]!, id: 'other', priority: 200 }],
    rules: [
      policy().rules[0]!,
      { ...policy().rules[0]!, id: 'probe-0002', group: 'other', match: '$req.path == "/bad"' },
    ],
  })
  const validator = {
    available: true,
    contractVersion: 1,
    revision: 'deadbeef',
    limits: null,
    validate: async (request: NativeValidationRequest): Promise<NativeValidationResult> => {
      const text = Buffer.from(request.payload).toString('utf8')
      const bad = text.indexOf('$req.path == "/bad"')
      return {
        valid: bad < 0,
        errors: bad < 0 ? [] : [{ code: 'script_compile_failed', offset: bad, message: 'nope' }],
        contractVersion: 1,
        validatorRevision: 'deadbeef',
      }
    },
  } as unknown as NativeValidator
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake(),
    validator,
  )

  const result = await service.compilePreview(actor, input, 'request-1')
  assert.equal(result.scriptValid, false)
  assert.deepEqual(
    result.scriptErrors.map(({ functionName, field }) => [functionName, field]),
    [['waf-other', 'rules[1].match']],
  )
})

test('compilePreview only revalidates the scripts an edit changed', async () => {
  const { validator, requests } = validatorFake()
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake(),
    validator,
  )
  const withOtherMatch = (match: string) =>
    policy({
      groups: [...policy().groups, { ...policy().groups[0]!, id: 'other' }],
      rules: [
        policy().rules[0]!,
        { ...policy().rules[0]!, id: 'probe-0002', group: 'other', match },
      ],
    })

  await service.compilePreview(actor, withOtherMatch('$req.path == "/a"'), 'request-1')
  assert.equal(requests.length, 2)
  await service.compilePreview(actor, withOtherMatch('$req.path == "/b"'), 'request-2')
  assert.equal(requests.length, 3)
  assert.ok(Buffer.from(requests[2]!.payload).toString('utf8').includes('"/b"'))
})

test('compilePreview surfaces native script compile errors', async () => {
  const { validator } = validatorFake({
    valid: false,
    errors: [{ code: 'script_compile_failed', offset: 42, message: 'unexpected escape' }],
  })
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake(),
    validator,
  )

  const result = await service.compilePreview(actor, policy(), 'request-1')
  assert.equal(result.valid, true)
  assert.equal(result.scriptValid, false)
  assert.deepEqual(result.scriptErrors, [
    {
      code: 'script_compile_failed',
      offset: 42,
      message: 'unexpected escape',
      functionName: 'waf-core',
    },
  ])
  assert.equal(result.compilation?.kind, 'mounted')
})

test('compilePreview attributes script errors inside a rule block to that rule', async () => {
  const input = policy()
  const compilation = compileWafPolicy(input)
  assert.equal(compilation.kind, 'mounted')
  if (compilation.kind !== 'mounted') {
    return
  }
  const offset = compilation.functions[0]!.ruleSpans[0]!.matchStart + 'strings.'.length
  const { validator } = validatorFake({
    valid: false,
    errors: [
      { code: 'script_compile_failed', field: 'payloadBase64', offset, message: 'not defined' },
    ],
  })
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake(),
    validator,
  )

  const result = await service.compilePreview(actor, input, 'request-1')
  assert.deepEqual(result.scriptErrors, [
    {
      code: 'script_compile_failed',
      functionName: 'waf-core',
      field: 'rules[0].match',
      offset,
      line: 1,
      column: 'strings.'.length + 1,
      message: 'not defined',
    },
  ])
})

test('compilePreview treats unmount candidates as valid with no script to compile', async () => {
  const { validator, requests } = validatorFake()
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake(),
    validator,
  )

  const result = await service.compilePreview(actor, policy({ rules: [] }), 'request-1')
  assert.equal(result.valid, true)
  assert.deepEqual(result.compilation, {
    kind: 'unmount',
    reason: 'no-rules',
    skipped: [{ groupId: 'core', reason: 'no-rules' }],
  })
  assert.equal(result.scriptValid, null)
  assert.deepEqual(requests, [])
})

test('compilePreview requires an editor role', async () => {
  const service = new DefaultWafService(
    {} as unknown as WafPolicyDraftRepository,
    environmentsFake('viewer'),
    validatorFake().validator,
  )
  await assert.rejects(
    service.compilePreview(actor, policy(), 'request-1'),
    (error: unknown) => error instanceof AppError && error.statusCode === 403,
  )
})

test('UnavailableWafService rejects every operation as unavailable', async () => {
  const service = new UnavailableWafService()
  const expectsUnavailable = (error: unknown): boolean =>
    error instanceof AppError && error.statusCode === 503
  await assert.rejects(service.getPolicyDraft(), expectsUnavailable)
  await assert.rejects(service.createPolicyDraft(), expectsUnavailable)
  await assert.rejects(service.savePolicyDraft(), expectsUnavailable)
  await assert.rejects(service.compilePreview(), expectsUnavailable)
})
