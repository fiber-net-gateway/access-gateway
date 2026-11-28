import type { FastifyInstance } from 'fastify'

import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import { GRAY_RULE_ENTRIES } from './model.js'
import type { GrayRuleReleaseService } from './release-service.js'
import type { GrayRuleService, GrayValidationPreviewView } from './service.js'

function parseIfMatch(value: string | undefined): string {
  if (!value) {
    throw badRequest('IF_MATCH_REQUIRED', 'If-Match is required when saving gray rules')
  }
  const match = /^(?:W\/)?"(0|[1-9][0-9]*)"$/u.exec(value.trim())
  if (!match?.[1]) {
    throw badRequest('INVALID_IF_MATCH', 'If-Match must contain the gray rules lock version ETag')
  }
  return match[1]
}

function parseIdempotencyKey(value: string | undefined): string {
  const key = value?.trim() ?? ''
  if (key.length < 1 || key.length > 128 || !/^[\x21-\x7e]+$/u.test(key)) {
    throw badRequest(
      'INVALID_IDEMPOTENCY_KEY',
      'Idempotency-Key must contain 1-128 printable ASCII characters',
    )
  }
  return key
}

const grayRuleSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['entry', 'ratio', 'cidrs'],
  properties: {
    entry: { type: 'string', enum: [...GRAY_RULE_ENTRIES] },
    ratio: { type: 'integer', minimum: 0, maximum: 10000 },
    cidrs: {
      type: 'array',
      maxItems: 4096,
      items: { type: 'string', minLength: 1, maxLength: 128 },
    },
  },
} as const

const grayRulesDocumentSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['schemaVersion', 'rules'],
  properties: {
    schemaVersion: { type: 'integer', const: 1 },
    rules: { type: 'array', maxItems: 64, items: grayRuleSchema },
  },
} as const

const grayRulesStructuralDocumentSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['schemaVersion', 'rules'],
  properties: {
    schemaVersion: { type: 'integer' },
    rules: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['entry', 'ratio', 'cidrs'],
        properties: {
          entry: { type: 'string' },
          ratio: { type: 'number' },
          cidrs: { type: 'array', items: { type: 'string' } },
        },
      },
    },
  },
} as const

const grayDraftResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id', 'document', 'documentSha256', 'lockVersion', 'createdAt', 'updatedAt'],
  properties: {
    id: { type: 'string', format: 'uuid' },
    document: grayRulesDocumentSchema,
    documentSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    createdAt: { type: 'string', format: 'date-time' },
    updatedAt: { type: 'string', format: 'date-time' },
  },
} as const

const grayValidationPreviewResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'valid',
    'issues',
    'wirePreview',
    'wireSha256',
    'dataId',
    'group',
    'ruleCount',
    'nativeValid',
    'nativeErrors',
    'validator',
  ],
  properties: {
    valid: { type: 'boolean' },
    issues: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['field', 'code', 'message'],
        properties: {
          field: { type: 'string' },
          code: { type: 'string' },
          message: { type: 'string' },
        },
      },
    },
    wirePreview: { type: ['string', 'null'] },
    wireSha256: { type: ['string', 'null'], pattern: '^[0-9a-f]{64}$' },
    dataId: { type: 'string' },
    group: { type: 'string' },
    ruleCount: { type: 'integer', minimum: 0 },
    nativeValid: { type: ['boolean', 'null'] },
    nativeErrors: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['code', 'message'],
        properties: {
          code: { type: 'string' },
          field: { type: 'string' },
          offset: { type: 'integer' },
          message: { type: 'string' },
        },
      },
    },
    validator: {
      anyOf: [
        { type: 'null' },
        {
          type: 'object',
          additionalProperties: false,
          required: ['contractVersion', 'revision'],
          properties: {
            contractVersion: { type: 'integer' },
            revision: { type: 'string' },
          },
        },
      ],
    },
  },
} as const

const activationSummarySchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'status',
    'targetCount',
    'activeCount',
    'pendingCount',
    'degradedCount',
    'unknownCount',
    'evaluatedAt',
  ],
  properties: {
    status: { type: 'string', enum: ['unknown', 'pending', 'active', 'degraded'] },
    targetCount: { type: 'integer', minimum: 0 },
    activeCount: { type: 'integer', minimum: 0 },
    pendingCount: { type: 'integer', minimum: 0 },
    degradedCount: { type: 'integer', minimum: 0 },
    unknownCount: { type: 'integer', minimum: 0 },
    evaluatedAt: { type: ['string', 'null'], format: 'date-time' },
  },
} as const

const grayReleaseResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'sequence',
    'status',
    'ruleCount',
    'entries',
    'wireSha256',
    'description',
    'resource',
    'publication',
    'activationStatus',
    'activation',
    'createdAt',
    'publishedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    sequence: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    status: {
      type: 'string',
      enum: [
        'creating',
        'validating',
        'validation_failed',
        'ready',
        'queued',
        'publishing',
        'published',
        'partially_published',
        'publish_failed',
        'canceled',
        'superseded',
        'abandoned',
      ],
    },
    ruleCount: { type: 'integer', minimum: 0 },
    entries: {
      type: 'array',
      items: { type: 'string', enum: [...GRAY_RULE_ENTRIES] },
    },
    wireSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    description: { type: 'string' },
    resource: {
      type: 'object',
      additionalProperties: false,
      required: ['id', 'dataId', 'group', 'status', 'verifiedSha256', 'verifiedAt'],
      properties: {
        id: { type: 'string', format: 'uuid' },
        dataId: { type: 'string' },
        group: { type: 'string' },
        status: {
          type: 'string',
          enum: ['pending', 'running', 'verified', 'failed', 'conflict', 'conflict_after_partial'],
        },
        verifiedSha256: { type: ['string', 'null'], pattern: '^[0-9a-f]{64}$' },
        verifiedAt: { type: ['string', 'null'], format: 'date-time' },
      },
    },
    publication: {
      type: 'object',
      additionalProperties: false,
      required: ['jobId', 'state'],
      properties: {
        jobId: { type: ['string', 'null'], format: 'uuid' },
        state: { type: ['string', 'null'] },
      },
    },
    activationStatus: { type: 'string', enum: ['unknown', 'pending', 'active', 'degraded'] },
    activation: activationSummarySchema,
    createdAt: { type: 'string', format: 'date-time' },
    publishedAt: { type: ['string', 'null'], format: 'date-time' },
  },
} as const

const grayLiveResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['exists', 'dataId', 'group', 'sha256', 'md5', 'content', 'document', 'parseError'],
  properties: {
    exists: { type: 'boolean' },
    dataId: { type: 'string' },
    group: { type: 'string' },
    sha256: { type: ['string', 'null'] },
    md5: { type: ['string', 'null'] },
    content: { type: ['string', 'null'] },
    document: { anyOf: [grayRulesDocumentSchema, { type: 'null' }] },
    parseError: { type: ['string', 'null'] },
  },
} as const

const releaseParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['releaseId'],
  properties: { releaseId: { type: 'string', format: 'uuid' } },
} as const

const idempotencyHeaders = {
  type: 'object',
  required: ['idempotency-key'],
  properties: { 'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 } },
} as const

interface SaveGrayDraftBody {
  document: unknown
}

interface CreateGrayReleaseBody {
  description?: string
}

export function registerGrayRoutes(
  app: FastifyInstance,
  auth: AuthService,
  gray: GrayRuleService,
  grayReleases: GrayRuleReleaseService,
): void {
  app.get(
    '/api/gray/draft',
    { schema: { response: { 200: grayDraftResponseSchema } } },
    async (_request, reply) => {
      const result = await gray.getDraft(await requireActor(auth))
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.post(
    '/api/gray/draft',
    { schema: { response: { 201: grayDraftResponseSchema } } },
    async (request, reply) => {
      const result = await gray.createDraft(await requireActor(auth), String(request.id))
      reply.header('etag', `"${result.lockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.put<{ Headers: { 'if-match'?: string }; Body: SaveGrayDraftBody }>(
    '/api/gray/draft',
    {
      schema: {
        headers: {
          type: 'object',
          required: ['if-match'],
          properties: { 'if-match': { type: 'string' } },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['document'],
          properties: {
            document: grayRulesDocumentSchema,
          },
        },
        response: { 200: grayDraftResponseSchema },
      },
    },
    async (request, reply) => {
      const result = await gray.saveDraft(
        await requireActor(auth),
        {
          lockVersion: parseIfMatch(request.headers['if-match']),
          document: request.body.document,
        },
        String(request.id),
      )
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.post<{ Body: unknown }>(
    '/api/gray/validate',
    {
      schema: {
        body: grayRulesStructuralDocumentSchema,
        response: { 200: grayValidationPreviewResponseSchema },
      },
    },
    async (request, reply) => {
      const result: GrayValidationPreviewView = await gray.validatePreview(
        await requireActor(auth),
        request.body,
        String(request.id),
      )
      reply.header('cache-control', 'no-store')
      return result
    },
  )

  app.get(
    '/api/gray/live',
    { schema: { response: { 200: grayLiveResponseSchema } } },
    async (_request, reply) => {
      const view = await grayReleases.currentLive(await requireActor(auth))
      reply.header('cache-control', 'no-store')
      return view
    },
  )

  app.get(
    '/api/gray/releases',
    {
      schema: {
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: grayReleaseResponseSchema } },
          },
        },
      },
    },
    async () => grayReleases.list(await requireActor(auth)),
  )

  app.get<{ Params: { releaseId: string } }>(
    '/api/gray/releases/:releaseId',
    { schema: { params: releaseParameters, response: { 200: grayReleaseResponseSchema } } },
    async (request) => grayReleases.get(await requireActor(auth), request.params.releaseId),
  )

  app.post<{ Headers: { 'idempotency-key'?: string }; Body?: CreateGrayReleaseBody }>(
    '/api/gray/releases',
    {
      schema: {
        headers: idempotencyHeaders,
        body: {
          anyOf: [
            { type: 'null' },
            {
              type: 'object',
              additionalProperties: false,
              properties: {
                description: { type: 'string', maxLength: 255 },
              },
            },
          ],
        },
        response: { 201: grayReleaseResponseSchema },
      },
    },
    async (request, reply) => {
      const release = await grayReleases.create(
        await requireActor(auth),
        {
          idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']),
          ...(request.body?.description !== undefined
            ? { description: request.body.description }
            : {}),
        },
        String(request.id),
      )
      return reply.code(201).send(release)
    },
  )

  app.post<{ Params: { releaseId: string }; Headers: { 'idempotency-key'?: string } }>(
    '/api/gray/releases/:releaseId/rollback',
    {
      schema: {
        params: releaseParameters,
        headers: idempotencyHeaders,
        response: { 201: grayReleaseResponseSchema },
      },
    },
    async (request, reply) => {
      const release = await grayReleases.rollback(
        await requireActor(auth),
        request.params.releaseId,
        { idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']) },
        String(request.id),
      )
      return reply.code(201).send(release)
    },
  )

  app.post<{ Params: { releaseId: string }; Headers: { 'idempotency-key'?: string } }>(
    '/api/gray/releases/:releaseId/publication',
    {
      schema: {
        params: releaseParameters,
        headers: idempotencyHeaders,
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['jobId', 'state', 'release'],
            properties: {
              jobId: { type: 'string', format: 'uuid' },
              state: { type: 'string' },
              release: grayReleaseResponseSchema,
            },
          },
        },
      },
    },
    async (request) =>
      grayReleases.queuePublication(
        await requireActor(auth),
        request.params.releaseId,
        parseIdempotencyKey(request.headers['idempotency-key']),
        String(request.id),
      ),
  )
}
