import type { FastifyInstance } from 'fastify'

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
      type: 'object',
      additionalProperties: false,
      required: ['schemaVersion', 'kind', 'hosts', 'routes'],
      properties: {
        schemaVersion: { type: 'integer', const: 1 },
        kind: { type: 'string', const: 'project_route' },
        hosts: { type: 'array', items: {} },
        routes: { type: 'array', items: {} },
      },
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
            model: { type: 'object', additionalProperties: true },
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
}
