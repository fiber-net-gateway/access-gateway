import assert from 'node:assert/strict'
import test from 'node:test'

import Fastify from 'fastify'

import type {
  NativeValidationError,
  NativeValidator,
} from '../../integrations/native-validator/model.js'
import type { Actor } from '../auth/model.js'
import { FixedActorAuthService } from '../auth/service.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { EdgeFunctionService } from './edge-function-service.js'
import { compileWafPolicy } from './generator.js'
import type { WafHitsService } from './hits-service.js'
import type { WafPolicy } from './model.js'
import { WafPolicyDraftRepository } from './repository.js'
import type { WafReleaseService } from './release-service.js'
import { registerWafRoutes } from './routes.js'
import { DefaultWafService } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}

const rule = {
  id: 'probe-0001',
  name: 'probe',
  group: 'core',
  severity: 'high',
  priority: 100,
  action: 'block',
  phase: 'request_head',
  match: 'strings.contains($req.path, "../")',
}

function policy(rules: readonly unknown[]) {
  return {
    schemaVersion: 2,
    groups: [
      { id: 'core', name: 'core', mode: 'log', priority: 100, projects: { include: ['*'] } },
    ],
    rules,
    projectModes: {},
  }
}

// The real service and validation behind real route schemas: the response
// schema is what used to reject the service's own `compilation: null`.
function appWith(errors: readonly NativeValidationError[] = []) {
  const environments = {
    findWorkspace: async () => ({ id: '00000000-0000-4000-8000-00000000000a', name: 'workspace' }),
    internalIdForActor: async () => '11',
    role: async () => 'maintainer',
  } as unknown as EnvironmentRepository
  const validator = {
    available: true,
    contractVersion: 1,
    revision: 'deadbeef',
    limits: null,
    validate: async () => ({
      valid: errors.length === 0,
      errors,
      contractVersion: 1,
      validatorRevision: 'deadbeef',
    }),
  } as unknown as NativeValidator
  const app = Fastify()
  registerWafRoutes(
    app,
    new FixedActorAuthService(actor),
    new DefaultWafService({} as unknown as WafPolicyDraftRepository, environments, validator),
    {} as unknown as WafReleaseService,
    {} as unknown as WafHitsService,
    {} as unknown as EdgeFunctionService,
  )
  return app
}

test('compile-preview answers value-level mistakes with structured issues', async () => {
  const app = appWith()
  const response = await app.inject({
    method: 'POST',
    url: '/api/waf/policy/compile-preview',
    payload: policy([
      {
        ...rule,
        name: '',
        priority: 2_000_000,
        match: '   ',
        scope: { methods: ['get'], sourceCidrs: ['10.0.0.0/8192.168.0.0/16'] },
      },
    ]),
  })

  assert.equal(response.statusCode, 200)
  const body = response.json<{
    valid: boolean
    compilation: unknown
    issues: { field: string }[]
  }>()
  assert.equal(body.valid, false)
  assert.equal(body.compilation, null)
  assert.deepEqual(body.issues.map((issue) => issue.field).sort(), [
    'rules[0].match',
    'rules[0].name',
    'rules[0].priority',
    'rules[0].scope.methods',
    'rules[0].scope.sourceCidrs',
  ])
})

test('compile-preview still rejects a body with the wrong structure', async () => {
  const app = appWith()
  const response = await app.inject({
    method: 'POST',
    url: '/api/waf/policy/compile-preview',
    payload: policy([{ ...rule, scope: { paths: { not: 'a list' } } }]),
  })

  assert.equal(response.statusCode, 400)
})

test('compile-preview returns attributed script errors without exposing rule spans', async () => {
  const compilation = compileWafPolicy(policy([rule]) as WafPolicy)
  assert.equal(compilation.kind, 'mounted')
  if (compilation.kind !== 'mounted') {
    return
  }
  const offset = compilation.functions[0]!.ruleSpans[0]!.matchStart + 'strings.'.length
  const app = appWith([{ code: 'script_compile_failed', offset, message: 'not defined' }])
  const response = await app.inject({
    method: 'POST',
    url: '/api/waf/policy/compile-preview',
    payload: policy([rule]),
  })

  assert.equal(response.statusCode, 200)
  const body = response.json<{
    valid: boolean
    scriptValid: boolean
    compilation: { functions: Record<string, unknown>[] }
    scriptErrors: { functionName: string; field?: string; line?: number; column?: number }[]
  }>()
  assert.equal(body.valid, true)
  assert.equal(body.scriptValid, false)
  assert.equal(body.compilation.functions[0]?.ruleSpans, undefined)
  assert.equal(body.scriptErrors[0]?.functionName, 'waf-core')
  assert.equal(body.scriptErrors[0]?.field, 'rules[0].match')
  assert.equal(body.scriptErrors[0]?.line, 1)
  assert.equal(body.scriptErrors[0]?.column, 'strings.'.length + 1)
})

test('saving the draft keeps the strict policy schema', async () => {
  const app = appWith()
  const response = await app.inject({
    method: 'PUT',
    url: '/api/waf/policy',
    headers: { 'if-match': '"1"' },
    payload: { policy: policy([{ ...rule, name: '' }]) },
  })

  assert.equal(response.statusCode, 400)
})
