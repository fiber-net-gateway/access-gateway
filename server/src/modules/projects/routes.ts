import type { FastifyInstance } from 'fastify'

import type { AuthService } from '../auth/model.js'
import { requireActor } from '../auth/http.js'
import type { CreateProjectInput } from './model.js'
import type { ProjectService } from './service.js'

const environmentParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['envId'],
  properties: { envId: { type: 'string', format: 'uuid' } },
} as const

const projectParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['projectId'],
  properties: { projectId: { type: 'string', format: 'uuid' } },
} as const

const projectResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'environmentId',
    'name',
    'status',
    'lockVersion',
    'draft',
    'publishedVersion',
    'activationStatus',
    'createdAt',
    'updatedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    environmentId: { type: 'string', format: 'uuid' },
    name: { type: 'string' },
    status: { type: 'string', enum: ['active', 'archived'] },
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    draft: {
      anyOf: [
        { type: 'null' },
        {
          type: 'object',
          additionalProperties: false,
          required: ['id', 'state', 'revision', 'lockVersion'],
          properties: {
            id: { type: 'string', format: 'uuid' },
            state: { type: 'string' },
            revision: { type: 'integer', minimum: 0 },
            lockVersion: { type: 'string' },
          },
        },
      ],
    },
    publishedVersion: { anyOf: [{ type: 'integer' }, { type: 'null' }] },
    activationStatus: { type: 'string', const: 'unknown' },
    createdAt: { type: 'string', format: 'date-time' },
    updatedAt: { type: 'string', format: 'date-time' },
  },
} as const

export function registerProjectRoutes(
  app: FastifyInstance,
  auth: AuthService,
  projects: ProjectService,
): void {
  app.get<{ Params: { envId: string } }>(
    '/api/environments/:envId/projects',
    {
      schema: {
        params: environmentParameters,
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: projectResponseSchema } },
          },
        },
      },
    },
    async (request) => projects.list(await requireActor(auth), request.params.envId),
  )

  app.post<{ Params: { envId: string }; Body: CreateProjectInput }>(
    '/api/environments/:envId/projects',
    {
      schema: {
        params: environmentParameters,
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['name'],
          properties: { name: { type: 'string', minLength: 1, maxLength: 255 } },
        },
        response: {
          201: {
            type: 'object',
            additionalProperties: false,
            required: ['id'],
            properties: { id: { type: 'string', format: 'uuid' } },
          },
        },
      },
    },
    async (request, reply) => {
      const result = await projects.create(
        await requireActor(auth),
        request.params.envId,
        request.body,
        String(request.id),
      )
      return reply.code(201).send(result)
    },
  )

  app.get<{ Params: { projectId: string } }>(
    '/api/projects/:projectId',
    { schema: { params: projectParameters, response: { 200: projectResponseSchema } } },
    async (request, reply) => {
      const result = await projects.get(await requireActor(auth), request.params.projectId)
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )
}
