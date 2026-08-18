import type { FastifyInstance } from 'fastify'

import { fallbackAccessConfigLimits } from '../../integrations/native-validator/limits.js'
import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import type { DraftService } from './service.js'

function parseIfMatch(value: string | undefined): string {
  if (!value) {
    throw badRequest('IF_MATCH_REQUIRED', 'If-Match is required when saving a revision')
  }
  const match = /^(?:W\/)?"(0|[1-9][0-9]*)"$/u.exec(value.trim())
  if (!match?.[1]) {
    throw badRequest('INVALID_IF_MATCH', 'If-Match must contain the draft lock version ETag')
  }
  return match[1]
}

const projectParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['projectId'],
  properties: { projectId: { type: 'string', format: 'uuid' } },
} as const

const draftParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['draftId'],
  properties: { draftId: { type: 'string', format: 'uuid' } },
} as const

const revisionParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['draftId', 'revisionId'],
  properties: {
    draftId: { type: 'string', format: 'uuid' },
    revisionId: { type: 'string', format: 'uuid' },
  },
} as const

const draftResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'projectId',
    'state',
    'title',
    'currentRevision',
    'lockVersion',
    'createdAt',
    'updatedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    projectId: { type: 'string', format: 'uuid' },
    state: { type: 'string', enum: ['editing', 'validating', 'ready'] },
    title: { type: 'string' },
    currentRevision: { type: 'integer', minimum: 0 },
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    createdAt: { type: 'string', format: 'date-time' },
    updatedAt: { type: 'string', format: 'date-time' },
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

const revisionResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'draftId',
    'revision',
    'model',
    'modelSha256',
    'validationState',
    'changeSummary',
    'createdAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    draftId: { type: 'string', format: 'uuid' },
    revision: { type: 'integer', minimum: 1 },
    model: {
      ...projectRoutesModelSchema,
    },
    modelSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    validationState: { type: 'string', enum: ['not_run', 'pending', 'valid', 'invalid'] },
    changeSummary: { type: 'string' },
    createdAt: { type: 'string', format: 'date-time' },
  },
} as const

interface CreateRevisionBody {
  changeSummary?: string
  model: unknown
}

export function registerDraftRoutes(
  app: FastifyInstance,
  auth: AuthService,
  drafts: DraftService,
): void {
  app.get<{ Params: { projectId: string } }>(
    '/api/projects/:projectId/drafts',
    { schema: { params: projectParameters, response: { 200: draftResponseSchema } } },
    async (request, reply) => {
      const result = await drafts.get(await requireActor(auth), request.params.projectId)
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )

  app.post<{ Params: { projectId: string } }>(
    '/api/projects/:projectId/drafts',
    { schema: { params: projectParameters, response: { 201: draftResponseSchema } } },
    async (request, reply) => {
      const result = await drafts.getOrCreate(
        await requireActor(auth),
        request.params.projectId,
        String(request.id),
      )
      reply.header('etag', `"${result.lockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.post<{
    Params: { draftId: string }
    Headers: { 'if-match'?: string }
    Body: CreateRevisionBody
  }>(
    '/api/drafts/:draftId/revisions',
    {
      schema: {
        params: draftParameters,
        headers: {
          type: 'object',
          required: ['if-match'],
          properties: { 'if-match': { type: 'string' } },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['model'],
          properties: {
            changeSummary: { type: 'string', maxLength: 1024 },
            model: projectRoutesModelSchema,
          },
        },
        response: { 201: revisionResponseSchema },
      },
    },
    async (request, reply) => {
      const result = await drafts.createRevision(
        await requireActor(auth),
        request.params.draftId,
        {
          lockVersion: parseIfMatch(request.headers['if-match']),
          changeSummary: request.body.changeSummary ?? '',
          model: request.body.model,
        },
        String(request.id),
      )
      const draftLockVersion = (BigInt(parseIfMatch(request.headers['if-match'])) + 1n).toString()
      reply.header('etag', `"${draftLockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.get<{ Params: { draftId: string; revisionId: string } }>(
    '/api/drafts/:draftId/revisions/:revisionId',
    { schema: { params: revisionParameters, response: { 200: revisionResponseSchema } } },
    async (request) =>
      drafts.getRevision(
        await requireActor(auth),
        request.params.draftId,
        request.params.revisionId,
      ),
  )

  app.get<{ Params: { draftId: string } }>(
    '/api/drafts/:draftId/current-revision',
    { schema: { params: draftParameters, response: { 200: revisionResponseSchema } } },
    async (request) => drafts.getCurrentRevision(await requireActor(auth), request.params.draftId),
  )

  app.post<{ Params: { projectId: string }; Body: { model: unknown } }>(
    '/api/projects/:projectId/routes/validate',
    {
      schema: {
        params: projectParameters,
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['model'],
          properties: { model: projectRoutesModelSchema },
        },
        response: {
          200: {
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
          },
        },
      },
    },
    async (request) =>
      drafts.validate(
        await requireActor(auth),
        request.params.projectId,
        request.body.model,
        String(request.id),
      ),
  )
}
