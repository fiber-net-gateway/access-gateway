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
import type { EdgeFunctionDraftView } from './edge-function-repository.js'
import { EdgeFunctionDraftRepository } from './edge-function-repository.js'
import {
  DefaultEdgeFunctionService,
  UnavailableEdgeFunctionService,
  type EdgeFunctionService,
} from './edge-function-service.js'
import type { EdgeFunctionsDraft } from './edge-function-model.js'

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

function draft(overrides: Partial<EdgeFunctionsDraft> = {}): EdgeFunctionsDraft {
  return {
    schemaVersion: 1,
    functions: [
      {
        name: 'bot-trap',
        enabled: true,
        script:
          'if (($header.user_agent contains "zgrab")) { ctx.report("bot.hit", "zgrab"); }\nreturn true;',
        mounts: [{ projects: { include: ['*'] }, priority: 50 }],
      },
      {
        name: 'parked',
        enabled: false,
        script: 'return true;',
        mounts: [{ projects: { include: ['*'] } }],
      },
    ],
    ...overrides,
  }
}

const draftView: EdgeFunctionDraftView = {
  id: '00000000-0000-4000-8000-00000000000e',
  draft: draft(),
  draftSha256: '0'.repeat(64),
  lockVersion: '1',
  createdAt: '2026-11-07T00:00:00.000Z',
  updatedAt: '2026-11-07T00:00:00.000Z',
}

function draftsFake() {
  return {
    findByEnvironmentInternalId: async () => draftView,
    getOrCreate: async () => draftView,
    save: async () => ({ ...draftView, lockVersion: '2' }),
  } as unknown as EdgeFunctionDraftRepository
}

test('saveDraft rejects an invalid draft before persistence', async () => {
  let persisted = false
  const drafts = {
    save: async () => {
      persisted = true
      throw new Error('must not persist')
    },
  } as unknown as EdgeFunctionDraftRepository
  const { validator } = validatorFake()
  const service: EdgeFunctionService = new DefaultEdgeFunctionService(
    drafts,
    environmentsFake(),
    validator,
  )

  await assert.rejects(
    service.saveDraft(
      actor,
      { lockVersion: '1', draft: { schemaVersion: 1, functions: [{ name: 'waf' }] } },
      'request-1',
    ),
    (error: unknown) => {
      assert.ok(error instanceof AppError)
      assert.equal(error.code, 'INVALID_EDGE_FUNCTION_DRAFT')
      return true
    },
  )
  assert.equal(persisted, false)
})

test('saveDraft rejects viewers and stale lock versions', async () => {
  const { validator } = validatorFake()
  const viewerService: EdgeFunctionService = new DefaultEdgeFunctionService(
    draftsFake(),
    environmentsFake('viewer'),
    validator,
  )
  await assert.rejects(
    viewerService.saveDraft(actor, { lockVersion: '1', draft: draft() }, 'request-1'),
    (error: unknown) => error instanceof AppError && error.statusCode === 403,
  )

  const stale: EdgeFunctionDraftRepository = {
    save: async () => {
      throw new AppError('EDGE_FUNCTION_DRAFT_VERSION_CONFLICT', 'stale', 409)
    },
  } as unknown as EdgeFunctionDraftRepository
  const staleService: EdgeFunctionService = new DefaultEdgeFunctionService(
    stale,
    environmentsFake(),
    validator,
  )
  await assert.rejects(
    staleService.saveDraft(actor, { lockVersion: '1', draft: draft() }, 'request-1'),
    (error: unknown) => {
      assert.ok(error instanceof AppError)
      assert.equal(error.code, 'EDGE_FUNCTION_DRAFT_VERSION_CONFLICT')
      return true
    },
  )
})

test('compilePreview compiles only enabled functions and reports per-function results', async () => {
  const { validator, requests } = validatorFake()
  const service: EdgeFunctionService = new DefaultEdgeFunctionService(
    draftsFake(),
    environmentsFake(),
    validator,
  )

  const view = await service.compilePreview(actor, draft(), 'request-1')

  assert.equal(view.valid, true)
  assert.deepEqual(view.issues, [])
  assert.equal(view.functions.length, 1)
  assert.equal(view.functions[0]!.name, 'bot-trap')
  assert.equal(view.functions[0]!.scriptValid, true)
  assert.equal(requests.length, 1)
  assert.equal(requests[0]!.kind, 'edge_function_script')
  assert.equal(Buffer.from(requests[0]!.payload).toString('utf8'), draft().functions[0]!.script)
  assert.deepEqual(view.validator, { contractVersion: 1, revision: 'deadbeef' })
})

test('compilePreview surfaces native errors per function without stopping the chain', async () => {
  const scriptsSeen: string[] = []
  const validator = {
    available: true,
    contractVersion: 1,
    revision: 'deadbeef',
    limits: null,
    validate: async (request: NativeValidationRequest): Promise<NativeValidationResult> => {
      scriptsSeen.push(Buffer.from(request.payload).toString('utf8'))
      const valid = !Buffer.from(request.payload).toString('utf8').includes('broken')
      return {
        valid,
        errors: valid
          ? []
          : [{ code: 'script_compile_failed', message: 'unexpected token', offset: 3 }],
        contractVersion: 1,
        validatorRevision: 'deadbeef',
      }
    },
  } as unknown as NativeValidator
  const service: EdgeFunctionService = new DefaultEdgeFunctionService(
    draftsFake(),
    environmentsFake(),
    validator,
  )
  const withTwoEnabled: EdgeFunctionsDraft = {
    schemaVersion: 1,
    functions: [
      {
        name: 'good-one',
        enabled: true,
        script: 'return true;',
        mounts: [{ projects: { include: ['*'] } }],
      },
      {
        name: 'bad-one',
        enabled: true,
        script: 'broken (',
        mounts: [{ projects: { include: ['*'] } }],
      },
    ],
  }

  const view = await service.compilePreview(actor, withTwoEnabled, 'request-1')

  assert.equal(scriptsSeen.length, 2)
  assert.deepEqual(
    view.functions.map((fn) => [fn.name, fn.scriptValid]),
    [
      ['good-one', true],
      ['bad-one', false],
    ],
  )
  assert.deepEqual(view.functions[1]!.errors, [
    { code: 'script_compile_failed', offset: 3, message: 'unexpected token' },
  ])
})

test('compilePreview reports field-level issues without calling the validator', async () => {
  const { validator, requests } = validatorFake()
  const service: EdgeFunctionService = new DefaultEdgeFunctionService(
    draftsFake(),
    environmentsFake(),
    validator,
  )

  const view = await service.compilePreview(
    actor,
    { schemaVersion: 1, functions: [{ name: 'WAF', enabled: true, script: 'x', mounts: [] }] },
    'request-1',
  )

  assert.equal(view.valid, false)
  assert.ok(view.issues.length > 0)
  assert.deepEqual(view.functions, [])
  assert.equal(requests.length, 0)
})

test('the unavailable service fails closed', async () => {
  const unavailable: EdgeFunctionService = new UnavailableEdgeFunctionService()
  await assert.rejects(unavailable.getDraft(actor), /MySQL is not configured/)
  await assert.rejects(unavailable.saveDraft(actor, { lockVersion: '0', draft: {} }, 'r'), /MySQL/)
  await assert.rejects(unavailable.compilePreview(actor, {}, 'r'), /MySQL/)
})
