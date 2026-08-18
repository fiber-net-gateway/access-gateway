import type { FastifyInstance } from 'fastify'

import { fallbackAccessConfigLimits } from '../../integrations/native-validator/limits.js'
import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import { releaseStatuses } from '../releases/state.js'
import type { ConfigurationVersionService } from './service.js'

function parseIfMatch(value: string | undefined): string {
  if (!value) throw badRequest('IF_MATCH_REQUIRED', 'If-Match is required when saving a version')
  const match = /^(?:W\/)?"(0|[1-9][0-9]*)"$/u.exec(value.trim())
  if (!match?.[1]) {
    throw badRequest('INVALID_IF_MATCH', 'If-Match must contain the configuration lock ETag')
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

const projectParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['projectId'],
  properties: { projectId: { type: 'string', format: 'uuid' } },
} as const

const versionParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['projectId', 'versionId'],
  properties: {
    projectId: { type: 'string', format: 'uuid' },
    versionId: { type: 'string', format: 'uuid' },
  },
} as const

const projectRoutesModelSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['schemaVersion', 'kind', 'networkPolicy', 'routes'],
  anyOf: [
    { properties: { schemaVersion: { const: 5 } }, not: { required: ['hostAliases'] } },
    { properties: { schemaVersion: { const: 6 } }, required: ['hostAliases'] },
  ],
  properties: {
    schemaVersion: { type: 'integer', enum: [5, 6] },
    kind: { type: 'string', const: 'project_routes_yaml' },
    hostAliases: {
      type: 'array',
      maxItems: Math.max(fallbackAccessConfigLimits.projectRoute.maxHosts - 1, 0),
      items: { type: 'string', minLength: 1, maxLength: 253 },
    },
    networkPolicy: {
      type: 'object',
      additionalProperties: false,
      required: ['source', 'httpsRedirect', 'allowedCidrs', 'deniedCidrs'],
      properties: {
        source: { type: 'string', enum: ['route', 'project'] },
        httpsRedirect: { type: 'string', enum: ['off', '301', '302', '307', '308'] },
        allowedCidrs: {
          type: 'array',
          maxItems: fallbackAccessConfigLimits.projectRoute.maxCidrsPerRoute,
          items: {
            type: 'string',
            minLength: 1,
            maxLength: fallbackAccessConfigLimits.projectRoute.maxCidrBytes,
          },
        },
        deniedCidrs: {
          type: 'array',
          maxItems: fallbackAccessConfigLimits.projectRoute.maxCidrsPerRoute,
          items: {
            type: 'string',
            minLength: 1,
            maxLength: fallbackAccessConfigLimits.projectRoute.maxCidrBytes,
          },
        },
      },
    },
    routes: {
      type: 'array',
      maxItems: fallbackAccessConfigLimits.projectRoute.maxRoutes,
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['id', 'format', 'source'],
        properties: {
          id: { type: 'string', format: 'uuid' },
          format: { type: 'string', enum: ['yaml', 'js'] },
          source: {
            type: 'string',
            maxLength: fallbackAccessConfigLimits.projectRoute.maxPayloadBytes,
          },
          path: {
            type: 'string',
            minLength: 1,
            maxLength: fallbackAccessConfigLimits.projectRoute.maxPathBytes,
          },
          method: {
            type: 'string',
            minLength: 1,
            maxLength: fallbackAccessConfigLimits.projectRoute.maxMethodBytes,
          },
          gzip: {
            anyOf: [{ type: 'boolean' }, { type: 'integer', minimum: 1, maximum: 9 }],
          },
        },
        if: { properties: { format: { const: 'js' } }, required: ['format'] },
        then: {
          required: ['path'],
          properties: {
            source: {
              type: 'string',
              maxLength: fallbackAccessConfigLimits.projectRoute.maxScriptBytes,
            },
          },
        },
        else: { not: { anyOf: [{ required: ['path'] }, { required: ['method'] }] } },
      },
    },
  },
} as const

const versionSummarySchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'projectId',
    'number',
    'relation',
    'baseVersionId',
    'restoredFromVersionId',
    'changeSummary',
    'routeCount',
    'modelSha256',
    'validationState',
    'publicationStatus',
    'createdBy',
    'createdAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    projectId: { type: 'string', format: 'uuid' },
    number: { type: 'integer', minimum: 1 },
    relation: { type: 'string', enum: ['current', 'historical'] },
    baseVersionId: { anyOf: [{ type: 'string', format: 'uuid' }, { type: 'null' }] },
    restoredFromVersionId: {
      anyOf: [{ type: 'string', format: 'uuid' }, { type: 'null' }],
    },
    changeSummary: { type: 'string' },
    routeCount: { type: 'integer', minimum: 0 },
    modelSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    validationState: { type: 'string', enum: ['not_run', 'pending', 'valid', 'invalid'] },
    publicationStatus: { type: 'string', enum: ['never', ...releaseStatuses] },
    createdBy: {
      type: 'object',
      additionalProperties: false,
      required: ['id', 'displayName'],
      properties: {
        id: { type: 'string', format: 'uuid' },
        displayName: { type: 'string' },
      },
    },
    createdAt: { type: 'string', format: 'date-time' },
  },
} as const

const versionDetailSchema = {
  ...versionSummarySchema,
  required: [...versionSummarySchema.required, 'model'],
  properties: { ...versionSummarySchema.properties, model: projectRoutesModelSchema },
} as const

const savedVersionSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['version', 'lockVersion'],
  properties: {
    version: versionDetailSchema,
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
  },
} as const

const validationSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['valid', 'issues', 'wirePreview', 'wireSha256', 'validator'],
  properties: {
    valid: { type: 'boolean' },
    issues: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['routeId', 'path', 'line', 'column', 'code', 'message'],
        properties: {
          routeId: { type: 'string', format: 'uuid' },
          path: { type: 'string' },
          line: { type: 'integer', minimum: 1 },
          column: { type: 'integer', minimum: 1 },
          code: { type: 'string' },
          message: { type: 'string' },
        },
      },
    },
    wirePreview: { anyOf: [{ type: 'string' }, { type: 'null' }] },
    wireSha256: {
      anyOf: [{ type: 'string', pattern: '^[0-9a-f]{64}$' }, { type: 'null' }],
    },
    validator: {
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
  },
} as const

interface SaveVersionBody {
  baseVersionId: string | null
  changeSummary: string
  forceSameContent?: boolean
  model: unknown
}

interface RestoreVersionBody {
  baseVersionId: string
  changeSummary: string
  forceSameContent?: boolean
  model?: unknown
}

export function registerConfigurationVersionRoutes(
  app: FastifyInstance,
  auth: AuthService,
  versions: ConfigurationVersionService,
): void {
  app.get<{
    Params: { projectId: string }
    Querystring: { cursor?: string; limit?: string }
  }>(
    '/api/projects/:projectId/configuration-versions',
    {
      schema: {
        params: projectParameters,
        querystring: {
          type: 'object',
          additionalProperties: false,
          properties: {
            cursor: { type: 'string', maxLength: 128 },
            limit: { type: 'integer', minimum: 1, maximum: 100, default: 50 },
          },
        },
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items', 'nextCursor', 'currentVersionId', 'lockVersion'],
            properties: {
              items: { type: 'array', items: versionSummarySchema },
              nextCursor: { anyOf: [{ type: 'string' }, { type: 'null' }] },
              currentVersionId: {
                anyOf: [{ type: 'string', format: 'uuid' }, { type: 'null' }],
              },
              lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
            },
          },
        },
      },
    },
    async (request) =>
      versions.list(
        await requireActor(auth),
        request.params.projectId,
        request.query.cursor,
        request.query.limit ? Number(request.query.limit) : undefined,
      ),
  )

  app.get<{ Params: { projectId: string } }>(
    '/api/projects/:projectId/configuration-versions/current',
    { schema: { params: projectParameters, response: { 200: savedVersionSchema } } },
    async (request, reply) => {
      const result = await versions.getCurrent(await requireActor(auth), request.params.projectId)
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.get<{ Params: { projectId: string; versionId: string } }>(
    '/api/projects/:projectId/configuration-versions/:versionId',
    { schema: { params: versionParameters, response: { 200: versionDetailSchema } } },
    async (request) =>
      versions.get(await requireActor(auth), request.params.projectId, request.params.versionId),
  )

  app.post<{
    Params: { projectId: string }
    Headers: { 'if-match'?: string; 'idempotency-key'?: string }
    Body: SaveVersionBody
  }>(
    '/api/projects/:projectId/configuration-versions',
    {
      schema: {
        params: projectParameters,
        headers: {
          type: 'object',
          required: ['if-match', 'idempotency-key'],
          properties: {
            'if-match': { type: 'string' },
            'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 },
          },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['baseVersionId', 'changeSummary', 'model'],
          properties: {
            baseVersionId: { anyOf: [{ type: 'string', format: 'uuid' }, { type: 'null' }] },
            changeSummary: { type: 'string', minLength: 1, maxLength: 200 },
            forceSameContent: { type: 'boolean', default: false },
            model: projectRoutesModelSchema,
          },
        },
        response: { 201: savedVersionSchema },
      },
    },
    async (request, reply) => {
      const result = await versions.save(
        await requireActor(auth),
        request.params.projectId,
        {
          lockVersion: parseIfMatch(request.headers['if-match']),
          baseVersionId: request.body.baseVersionId,
          changeSummary: request.body.changeSummary,
          forceSameContent: request.body.forceSameContent ?? false,
          idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']),
          model: request.body.model,
        },
        String(request.id),
      )
      reply.header('etag', `"${result.lockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.post<{
    Params: { projectId: string; versionId: string }
    Headers: { 'if-match'?: string; 'idempotency-key'?: string }
    Body: RestoreVersionBody
  }>(
    '/api/projects/:projectId/configuration-versions/:versionId/restorations',
    {
      schema: {
        params: versionParameters,
        headers: {
          type: 'object',
          required: ['if-match', 'idempotency-key'],
          properties: {
            'if-match': { type: 'string' },
            'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 },
          },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['baseVersionId', 'changeSummary'],
          properties: {
            baseVersionId: { type: 'string', format: 'uuid' },
            changeSummary: { type: 'string', minLength: 1, maxLength: 200 },
            forceSameContent: { type: 'boolean', default: false },
            model: projectRoutesModelSchema,
          },
        },
        response: { 201: savedVersionSchema },
      },
    },
    async (request, reply) => {
      const result = await versions.restore(
        await requireActor(auth),
        request.params.projectId,
        request.params.versionId,
        {
          lockVersion: parseIfMatch(request.headers['if-match']),
          baseVersionId: request.body.baseVersionId,
          changeSummary: request.body.changeSummary,
          forceSameContent: request.body.forceSameContent ?? false,
          idempotencyKey: parseIdempotencyKey(request.headers['idempotency-key']),
          model: request.body.model,
        },
        String(request.id),
      )
      reply.header('etag', `"${result.lockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.post<{ Params: { projectId: string; versionId: string } }>(
    '/api/projects/:projectId/configuration-versions/:versionId/validations',
    {
      schema: {
        params: versionParameters,
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['version', 'validation'],
            properties: { version: versionSummarySchema, validation: validationSchema },
          },
        },
      },
    },
    async (request) =>
      versions.validate(
        await requireActor(auth),
        request.params.projectId,
        request.params.versionId,
        String(request.id),
      ),
  )
}
