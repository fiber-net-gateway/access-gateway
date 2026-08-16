import type { FastifyInstance } from 'fastify'

import { badRequest } from '../../shared/errors.js'
import { activationSummarySchema } from '../activation/routes.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import type { ReleaseService } from './service.js'
import { releaseStatuses } from './state.js'

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

function parseIfMatch(value: string | undefined): string {
  if (!value) {
    throw badRequest('IF_MATCH_REQUIRED', 'If-Match is required when decommissioning a Project')
  }
  const match = /^"(0|[1-9][0-9]*)"$/u.exec(value.trim())
  if (!match?.[1]) {
    throw badRequest('INVALID_IF_MATCH', 'If-Match must contain the Project lock ETag')
  }
  return match[1]
}

const projectParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['projectId'],
  properties: { projectId: { type: 'string', format: 'uuid' } },
} as const

const releaseParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['releaseId'],
  properties: { releaseId: { type: 'string', format: 'uuid' } },
} as const

const releaseResourceSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'kind',
    'dataId',
    'group',
    'operation',
    'status',
    'targetSha256',
    'verifiedSha256',
    'verifiedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    kind: { type: 'string', enum: ['project_route', 'project_list'] },
    dataId: { type: 'string' },
    group: { type: 'string' },
    operation: { type: 'string', enum: ['upsert', 'remove'] },
    status: {
      type: 'string',
      enum: ['pending', 'running', 'verified', 'failed', 'conflict', 'conflict_after_partial'],
    },
    targetSha256: {
      anyOf: [{ type: 'string', pattern: '^[0-9a-f]{64}$' }, { type: 'null' }],
    },
    verifiedSha256: {
      anyOf: [{ type: 'string', pattern: '^[0-9a-f]{64}$' }, { type: 'null' }],
    },
    verifiedAt: { anyOf: [{ type: 'string', format: 'date-time' }, { type: 'null' }] },
  },
} as const

const configurationVersionReferenceSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id', 'number'],
  properties: {
    id: { type: 'string', format: 'uuid' },
    number: { type: 'integer', minimum: 1 },
  },
} as const

const releaseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'sequence',
    'projectId',
    'kind',
    'title',
    'description',
    'status',
    'sourceConfigurationVersion',
    'currentConfigurationVersionAtCreation',
    'allocatedWireVersion',
    'sourceModelSha256',
    'wireSha256',
    'nativeValidator',
    'compilerRevision',
    'validationErrors',
    'resources',
    'publication',
    'activationStatus',
    'activation',
    'createdAt',
    'publishedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    sequence: { type: 'string', pattern: '^[1-9][0-9]*$' },
    projectId: { type: 'string', format: 'uuid' },
    kind: { type: 'string', enum: ['project_route', 'project_decommission'] },
    title: { type: 'string' },
    description: { type: 'string' },
    status: { type: 'string', enum: releaseStatuses },
    sourceConfigurationVersion: {
      anyOf: [
        { type: 'null' },
        {
          ...configurationVersionReferenceSchema,
          required: ['id', 'number', 'relationAtCreation'],
          properties: {
            ...configurationVersionReferenceSchema.properties,
            relationAtCreation: { type: 'string', enum: ['current', 'historical', 'unknown'] },
          },
        },
      ],
    },
    currentConfigurationVersionAtCreation: {
      anyOf: [{ type: 'null' }, configurationVersionReferenceSchema],
    },
    allocatedWireVersion: {
      anyOf: [{ type: 'integer', minimum: 1 }, { type: 'null' }],
    },
    sourceModelSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    wireSha256: {
      anyOf: [{ type: 'string', pattern: '^[0-9a-f]{64}$' }, { type: 'null' }],
    },
    nativeValidator: {
      anyOf: [
        { type: 'null' },
        {
          type: 'object',
          additionalProperties: false,
          required: ['contractVersion', 'revision'],
          properties: {
            contractVersion: { type: 'integer', minimum: 1 },
            revision: { type: 'string' },
          },
        },
      ],
    },
    compilerRevision: { anyOf: [{ type: 'string' }, { type: 'null' }] },
    validationErrors: { type: 'array', items: {} },
    resources: { type: 'array', items: releaseResourceSchema },
    publication: {
      type: 'object',
      additionalProperties: false,
      required: ['jobId', 'state'],
      properties: {
        jobId: { anyOf: [{ type: 'string', format: 'uuid' }, { type: 'null' }] },
        state: { anyOf: [{ type: 'string' }, { type: 'null' }] },
      },
    },
    activationStatus: { type: 'string', enum: ['unknown', 'pending', 'active', 'degraded'] },
    activation: activationSummarySchema,
    createdAt: { type: 'string', format: 'date-time' },
    publishedAt: { anyOf: [{ type: 'string', format: 'date-time' }, { type: 'null' }] },
  },
} as const

export function registerReleaseRoutes(
  app: FastifyInstance,
  auth: AuthService,
  releases: ReleaseService,
): void {
  app.get<{ Params: { projectId: string } }>(
    '/api/projects/:projectId/releases',
    {
      schema: {
        params: projectParameters,
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: releaseSchema } },
          },
        },
      },
    },
    async (request) => releases.list(await requireActor(auth), request.params.projectId),
  )

  app.get<{ Params: { releaseId: string } }>(
    '/api/releases/:releaseId',
    { schema: { params: releaseParameters, response: { 200: releaseSchema } } },
    async (request) => releases.get(await requireActor(auth), request.params.releaseId),
  )

  app.post<{
    Params: { projectId: string }
    Headers: { 'idempotency-key'?: string }
    Body: {
      sourceVersionId: string
      expectedCurrentVersionId: string
      title: string
      description?: string
    }
  }>(
    '/api/projects/:projectId/releases',
    {
      schema: {
        params: projectParameters,
        headers: {
          type: 'object',
          required: ['idempotency-key'],
          properties: { 'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 } },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['sourceVersionId', 'expectedCurrentVersionId', 'title'],
          properties: {
            sourceVersionId: { type: 'string', format: 'uuid' },
            expectedCurrentVersionId: { type: 'string', format: 'uuid' },
            title: { type: 'string', minLength: 1, maxLength: 255 },
            description: { type: 'string', maxLength: 4000, default: '' },
          },
        },
        response: { 201: releaseSchema },
      },
    },
    async (request, reply) => {
      const release = await releases.create(
        await requireActor(auth),
        request.params.projectId,
        {
          sourceVersionId: request.body.sourceVersionId,
          expectedCurrentVersionId: request.body.expectedCurrentVersionId,
          title: request.body.title,
          description: request.body.description ?? '',
          idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']),
        },
        String(request.id),
      )
      return reply.code(201).send(release)
    },
  )

  app.post<{
    Params: { projectId: string }
    Headers: { 'if-match'?: string; 'idempotency-key'?: string }
    Body: { confirmationDomain: string; reason: string }
  }>(
    '/api/projects/:projectId/decommission-releases',
    {
      schema: {
        params: projectParameters,
        headers: {
          type: 'object',
          required: ['if-match', 'idempotency-key'],
          properties: {
            'if-match': { type: 'string', minLength: 3, maxLength: 32 },
            'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 },
          },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['confirmationDomain', 'reason'],
          properties: {
            confirmationDomain: { type: 'string', minLength: 1, maxLength: 255 },
            reason: { type: 'string', minLength: 1, maxLength: 1000 },
          },
        },
        response: { 201: releaseSchema },
      },
    },
    async (request, reply) => {
      const release = await releases.createDecommission(
        await requireActor(auth),
        request.params.projectId,
        {
          confirmationDomain: request.body.confirmationDomain,
          reason: request.body.reason,
          expectedLockVersion: parseIfMatch(request.headers['if-match']),
          idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']),
        },
        String(request.id),
      )
      return reply.code(201).send(release)
    },
  )

  app.post<{
    Params: { releaseId: string }
    Headers: { 'idempotency-key'?: string }
  }>(
    '/api/releases/:releaseId/publications',
    {
      schema: {
        params: releaseParameters,
        headers: {
          type: 'object',
          required: ['idempotency-key'],
          properties: { 'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 } },
        },
        response: {
          202: {
            type: 'object',
            additionalProperties: false,
            required: ['jobId', 'state', 'release'],
            properties: {
              jobId: { type: 'string', format: 'uuid' },
              state: { type: 'string' },
              release: releaseSchema,
            },
          },
        },
      },
    },
    async (request, reply) => {
      const result = await releases.queuePublication(
        await requireActor(auth),
        request.params.releaseId,
        parseIdempotencyKey(request.headers['idempotency-key']),
        String(request.id),
      )
      return reply.code(202).send(result)
    },
  )
}
