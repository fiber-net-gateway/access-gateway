import assert from 'node:assert/strict'
import test from 'node:test'

import Fastify from 'fastify'

import type { NacosClient, NacosResourceValue } from '../../integrations/nacos/model.js'
import type {
  NativeValidationError,
  NativeValidator,
} from '../../integrations/native-validator/model.js'
import { AppError } from '../../shared/errors.js'
import { sha256 } from '../../shared/json.js'
import type { Actor } from '../auth/model.js'
import { FixedActorAuthService } from '../auth/service.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { unknownActivationSummary } from '../activation/model.js'
import {
  compileGrayRulesWire,
  GRAY_MATCH_DATA_ID,
  GRAY_MATCH_GROUP,
  type GrayRulesDocument,
} from './model.js'
import type { GrayReleaseView } from './release-model.js'
import {
  GrayRuleReleaseRepository,
  type GrayRollbackSource,
  type PreparedGrayReleasePlan,
} from './release-repository.js'
import { DefaultGrayRuleReleaseService } from './release-service.js'
import { GrayRuleDraftRepository, type GrayRulesDraftView } from './repository.js'
import { registerGrayRoutes } from './routes.js'
import { DefaultGrayRuleService } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test-operator',
  displayName: 'Test Operator',
  platformAdmin: false,
}

const releaseId = '50000000-0000-4000-8000-000000000010'
const rollbackReleaseId = '50000000-0000-4000-8000-000000000011'
const resourceId = '60000000-0000-4000-8000-000000000010'
const jobId = '70000000-0000-4000-8000-000000000010'

const sampleDocument: GrayRulesDocument = {
  schemaVersion: 1,
  rules: [{ entry: 'internet', ratio: 500, cidrs: ['10.0.0.0/8', '2001:db8::/32'] }],
}

function sampleDraft(
  document: GrayRulesDocument = sampleDocument,
  lockVersion = '1',
): GrayRulesDraftView {
  return {
    id: '20000000-0000-4000-8000-000000000001',
    document,
    documentSha256: sha256(JSON.stringify(document)).toString('hex'),
    lockVersion,
    createdAt: '2026-11-28T00:00:00.000Z',
    updatedAt: '2026-11-28T00:00:00.000Z',
  }
}

function sampleRelease(overrides: Partial<GrayReleaseView> = {}): GrayReleaseView {
  const wire = compileGrayRulesWire(sampleDocument)
  return {
    id: releaseId,
    sequence: '12',
    status: 'ready',
    ruleCount: 1,
    entries: ['internet'],
    wireSha256: sha256(wire).toString('hex'),
    description: 'canary 5%',
    resource: {
      id: resourceId,
      dataId: GRAY_MATCH_DATA_ID,
      group: GRAY_MATCH_GROUP,
      status: 'pending',
      verifiedSha256: null,
      verifiedAt: null,
    },
    publication: { jobId: null, state: null },
    activationStatus: 'unknown',
    activation: unknownActivationSummary(),
    createdAt: '2026-11-28T00:00:00.000Z',
    publishedAt: null,
    ...overrides,
  }
}

function buildTestApp(
  options: {
    role?: 'viewer' | 'maintainer' | 'publisher' | 'admin'
    validatorAvailable?: boolean
    validatorErrors?: readonly NativeValidationError[]
    nacosAvailable?: boolean
    nacosLive?: NacosResourceValue
    draft?: GrayRulesDraftView | null
    rollbackSource?: GrayRollbackSource | null
  } = {},
) {
  const createdPlans: PreparedGrayReleasePlan[] = []
  let savedDocument: GrayRulesDocument | null = null
  const validatedKinds: string[] = []

  const environments = {
    findWorkspace: async () => ({
      id: '00000000-0000-4000-8000-00000000000a',
      code: 'prod',
      name: 'workspace',
      nacos: {
        endpoint: 'http://127.0.0.1:8848',
        namespace: '',
        tenant: 'public',
        credentialConfigured: false,
      },
    }),
    internalIdForActor: async () => '11',
    role: async () => options.role ?? 'admin',
  } as unknown as EnvironmentRepository

  const validator = {
    available: options.validatorAvailable ?? true,
    contractVersion: 1,
    revision: options.validatorAvailable === false ? null : 'validator-rev-1',
    limits: null,
    validate: async (request: { kind: string }) => {
      validatedKinds.push(request.kind)
      const errors = options.validatorErrors ?? []
      return {
        valid: errors.length === 0,
        errors,
        contractVersion: 1,
        validatorRevision: 'validator-rev-1',
      }
    },
  } as unknown as NativeValidator

  const nacos = {
    available: options.nacosAvailable ?? true,
    read: async (): Promise<NacosResourceValue> =>
      options.nacosLive ?? {
        exists: true,
        content: '{"internet":{"cidrs":["10.0.0.0/8"],"ratio":100}}',
        sha256: sha256('{"internet":{"cidrs":["10.0.0.0/8"],"ratio":100}}').toString('hex'),
        md5: '1'.repeat(32),
      },
  } as unknown as NacosClient

  const draftRepo = {
    findByEnvironmentInternalId: async () =>
      options.draft === undefined ? sampleDraft() : options.draft,
    getOrCreate: async () => sampleDraft({ schemaVersion: 1, rules: [] }, '0'),
    save: async (
      _actor: Actor,
      _envId: string,
      _lockVersion: string,
      document: GrayRulesDocument,
    ) => {
      savedDocument = document
      return sampleDraft(document, '2')
    },
  } as unknown as GrayRuleDraftRepository

  const releaseRepo = {
    create: async (
      _actor: Actor,
      _envId: string,
      plan: PreparedGrayReleasePlan,
      _base: NacosResourceValue,
      metadata: { description?: string; rollbackOfSequence?: string },
    ) => {
      createdPlans.push(plan)
      return sampleRelease({
        id: metadata.rollbackOfSequence ? rollbackReleaseId : releaseId,
        sequence: metadata.rollbackOfSequence ? '13' : '12',
        description: metadata.description ?? '',
        ruleCount: plan.ruleCount,
        entries: plan.entries,
        wireSha256: sha256(plan.wirePayload).toString('hex'),
      })
    },
    loadRollbackSource: async () =>
      options.rollbackSource === undefined
        ? {
            sequence: '10',
            compilerRevision: 'gray-match-v1',
            plan: {
              wirePayload: '{"vdi":{"cidrs":["10.1.0.0/16"],"ratio":200}}',
              ruleCount: 1,
              entries: ['vdi'],
            },
          }
        : options.rollbackSource,
    list: async () => [sampleRelease()],
    find: async () => sampleRelease(),
    queuePublication: async () => ({
      jobId,
      state: 'queued',
      release: sampleRelease({ status: 'queued', publication: { jobId, state: 'queued' } }),
    }),
  } as unknown as GrayRuleReleaseRepository

  const app = Fastify()
  app.setErrorHandler(async (error, request, reply) => {
    if (error instanceof AppError) {
      return reply.code(error.statusCode).send({
        error: {
          code: error.code,
          message: error.message,
          requestId: String(request.id),
          ...(error.fields.length > 0 ? { fields: error.fields } : {}),
        },
      })
    }
    return reply.code(400).send({
      error: {
        code: 'BAD_REQUEST',
        message: error instanceof Error ? error.message : 'Invalid request',
      },
    })
  })

  registerGrayRoutes(
    app,
    new FixedActorAuthService(actor),
    new DefaultGrayRuleService(draftRepo, environments, validator),
    new DefaultGrayRuleReleaseService(draftRepo, releaseRepo, environments, validator, nacos),
  )

  return { app, createdPlans, validatedKinds, getSavedDocument: () => savedDocument }
}

test('GET and PUT /api/gray/draft enforce ETag, RBAC, and CIDR validation', async () => {
  const { app, getSavedDocument } = buildTestApp({ role: 'maintainer' })

  const getRes = await app.inject({ method: 'GET', url: '/api/gray/draft' })
  assert.equal(getRes.statusCode, 200)
  assert.equal(getRes.headers.etag, '"1"')

  const invalidRes = await app.inject({
    method: 'PUT',
    url: '/api/gray/draft',
    headers: { 'if-match': '"1"' },
    payload: {
      document: {
        schemaVersion: 1,
        rules: [{ entry: 'internet', ratio: 500, cidrs: ['10.0.0.0/33'] }],
      },
    },
  })
  assert.equal(invalidRes.statusCode, 400)
  const invalidBody = invalidRes.json<{ error: { code: string; fields: { path: string }[] } }>()
  assert.equal(invalidBody.error.code, 'INVALID_GRAY_RULES')
  assert.equal(invalidBody.error.fields[0]?.path, 'rules[0].cidrs[0]')

  const saveRes = await app.inject({
    method: 'PUT',
    url: '/api/gray/draft',
    headers: { 'if-match': '"1"' },
    payload: { document: sampleDocument },
  })
  assert.equal(saveRes.statusCode, 200)
  assert.equal(saveRes.headers.etag, '"2"')
  assert.deepEqual(getSavedDocument(), sampleDocument)

  const viewerApp = buildTestApp({ role: 'viewer' }).app
  const forbiddenRes = await viewerApp.inject({
    method: 'PUT',
    url: '/api/gray/draft',
    headers: { 'if-match': '"1"' },
    payload: { document: sampleDocument },
  })
  assert.equal(forbiddenRes.statusCode, 403)
})

test('POST /api/gray/validate runs Native Validator (kind gray_rules) and reports unavailable honestly', async () => {
  const { app, validatedKinds } = buildTestApp()

  const valueErrorRes = await app.inject({
    method: 'POST',
    url: '/api/gray/validate',
    payload: {
      schemaVersion: 1,
      rules: [{ entry: 'internet', ratio: 20000, cidrs: ['bad-cidr'] }],
    },
  })
  assert.equal(valueErrorRes.statusCode, 200)
  const valueErrorBody = valueErrorRes.json<{
    valid: boolean
    issues: { field: string }[]
    wirePreview: string | null
  }>()
  assert.equal(valueErrorBody.valid, false)
  assert.equal(valueErrorBody.wirePreview, null)
  assert.deepEqual(
    valueErrorBody.issues.map((issue) => issue.field),
    ['rules[0].ratio', 'rules[0].cidrs[0]'],
  )

  const okRes = await app.inject({
    method: 'POST',
    url: '/api/gray/validate',
    payload: sampleDocument,
  })
  assert.equal(okRes.statusCode, 200)
  const okBody = okRes.json<{
    valid: boolean
    nativeValid: boolean | null
    wirePreview: string
    dataId: string
    group: string
  }>()
  assert.equal(okBody.valid, true)
  assert.equal(okBody.nativeValid, true)
  assert.equal(okBody.dataId, GRAY_MATCH_DATA_ID)
  assert.equal(okBody.group, GRAY_MATCH_GROUP)
  assert.equal(okBody.wirePreview, compileGrayRulesWire(sampleDocument))
  assert.deepEqual(validatedKinds, ['gray_rules'])

  const unavailableApp = buildTestApp({ validatorAvailable: false }).app
  const unavailRes = await unavailableApp.inject({
    method: 'POST',
    url: '/api/gray/validate',
    payload: sampleDocument,
  })
  assert.equal(unavailRes.statusCode, 200)
  assert.equal(unavailRes.json<{ nativeValid: boolean | null }>().nativeValid, null)
})

test('gray release create, publication queueing, rollback, and live read enforce fail-closed rules', async () => {
  const { app, createdPlans, validatedKinds } = buildTestApp({ role: 'publisher' })

  const createRes = await app.inject({
    method: 'POST',
    url: '/api/gray/releases',
    headers: { 'idempotency-key': 'gray-rel-1' },
    payload: { description: 'canary 5%' },
  })
  assert.equal(createRes.statusCode, 201)
  assert.equal(createdPlans.length, 1)
  assert.equal(createdPlans[0]?.wirePayload, compileGrayRulesWire(sampleDocument))
  assert.deepEqual(validatedKinds, ['gray_rules'])

  const pubRes = await app.inject({
    method: 'POST',
    url: `/api/gray/releases/${releaseId}/publication`,
    headers: { 'idempotency-key': 'gray-pub-1' },
  })
  assert.equal(pubRes.statusCode, 200)
  assert.equal(
    pubRes.json<{ state: string; release: { status: string } }>().release.status,
    'queued',
  )

  const rollbackRes = await app.inject({
    method: 'POST',
    url: `/api/gray/releases/${releaseId}/rollback`,
    headers: { 'idempotency-key': 'gray-rb-1' },
  })
  assert.equal(rollbackRes.statusCode, 201)
  const rbBody = rollbackRes.json<GrayReleaseView>()
  assert.equal(rbBody.id, rollbackReleaseId)
  assert.equal(rbBody.description, 'rollback of gray rules #10')
  assert.deepEqual(rbBody.entries, ['vdi'])

  const liveRes = await app.inject({ method: 'GET', url: '/api/gray/live' })
  assert.equal(liveRes.statusCode, 200)
  assert.equal(liveRes.headers['cache-control'], 'no-store')
  assert.equal(liveRes.json<{ exists: boolean; dataId: string }>().exists, true)

  // Fail closed when Native Validator rejects or is unavailable
  const rejectedApp = buildTestApp({
    role: 'publisher',
    validatorErrors: [{ code: 'invalid_cidr', field: 'rules[0].cidrs[0]', message: 'bad cidr' }],
  }).app
  const rejectedRes = await rejectedApp.inject({
    method: 'POST',
    url: '/api/gray/releases',
    headers: { 'idempotency-key': 'gray-rel-2' },
    payload: {},
  })
  assert.equal(rejectedRes.statusCode, 422)

  const unavailApp = buildTestApp({ role: 'publisher', validatorAvailable: false }).app
  const unavailRes = await unavailApp.inject({
    method: 'POST',
    url: '/api/gray/releases',
    headers: { 'idempotency-key': 'gray-rel-3' },
    payload: {},
  })
  assert.equal(unavailRes.statusCode, 503)

  // Maintainer cannot create releases
  const maintainerApp = buildTestApp({ role: 'maintainer' }).app
  const forbiddenCreate = await maintainerApp.inject({
    method: 'POST',
    url: '/api/gray/releases',
    headers: { 'idempotency-key': 'gray-rel-4' },
    payload: {},
  })
  assert.equal(forbiddenCreate.statusCode, 403)
})
