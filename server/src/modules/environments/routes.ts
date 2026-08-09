import type { FastifyInstance } from 'fastify'

import type { AuthService } from '../auth/model.js'
import { requireActor } from '../auth/http.js'
import type { CreateEnvironmentInput } from './model.js'
import type { EnvironmentService } from './service.js'

const uuidParameterSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['id'],
  properties: { id: { type: 'string', format: 'uuid' } },
} as const

const createEnvironmentSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['code', 'name', 'tier', 'nacosEndpoint'],
  properties: {
    code: { type: 'string', minLength: 1, maxLength: 64 },
    name: { type: 'string', minLength: 1, maxLength: 255 },
    tier: { type: 'string', enum: ['local', 'test', 'staging', 'production'] },
    nacosEndpoint: { type: 'string', minLength: 1, maxLength: 2048 },
    nacosNamespace: { type: 'string', maxLength: 255 },
    nacosTenant: { type: 'string', maxLength: 255 },
    zone: { type: 'string', maxLength: 255 },
    dataIds: {
      type: 'object',
      additionalProperties: false,
      properties: {
        projects: { type: 'string', maxLength: 512 },
        routePrefix: { type: 'string', maxLength: 512 },
        routeGroup: { type: 'string', maxLength: 255 },
        gray: { type: 'string', maxLength: 512 },
        grayGroup: { type: 'string', maxLength: 255 },
        namingGroup: { type: 'string', maxLength: 255 },
      },
    },
    protectionPolicy: { type: 'object', additionalProperties: true },
  },
} as const

const capabilityDataIdsSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['projects', 'routePrefix', 'routeGroup', 'gray', 'grayGroup', 'namingGroup'],
  properties: {
    projects: { type: 'string' },
    routePrefix: { type: 'string' },
    routeGroup: { type: 'string' },
    gray: { type: 'string' },
    grayGroup: { type: 'string' },
    namingGroup: { type: 'string' },
  },
} as const

const environmentResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'code',
    'name',
    'tier',
    'status',
    'nacos',
    'dataIds',
    'zone',
    'protectionPolicy',
    'lockVersion',
    'createdAt',
    'updatedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    code: { type: 'string' },
    name: { type: 'string' },
    tier: { type: 'string', enum: ['local', 'test', 'staging', 'production'] },
    status: { type: 'string', enum: ['active', 'disabled'] },
    nacos: {
      type: 'object',
      additionalProperties: false,
      required: ['endpoint', 'namespace', 'tenant', 'credentialConfigured'],
      properties: {
        endpoint: { type: 'string' },
        namespace: { type: 'string' },
        tenant: { type: 'string' },
        credentialConfigured: { type: 'boolean' },
      },
    },
    dataIds: capabilityDataIdsSchema,
    zone: { type: 'string' },
    protectionPolicy: { type: 'object', additionalProperties: true },
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    createdAt: { type: 'string', format: 'date-time' },
    updatedAt: { type: 'string', format: 'date-time' },
  },
} as const

export function registerEnvironmentRoutes(
  app: FastifyInstance,
  auth: AuthService,
  environments: EnvironmentService,
): void {
  app.get(
    '/api/environments',
    {
      schema: {
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: environmentResponseSchema } },
          },
        },
      },
    },
    async () => environments.list(await requireActor(auth)),
  )

  app.post<{ Body: CreateEnvironmentInput }>(
    '/api/environments',
    { schema: { body: createEnvironmentSchema, response: { 201: environmentResponseSchema } } },
    async (request, reply) => {
      const result = await environments.create(
        await requireActor(auth),
        request.body,
        String(request.id),
      )
      reply.header('etag', `"${result.lockVersion}"`)
      return reply.code(201).send(result)
    },
  )

  app.get<{ Params: { id: string } }>(
    '/api/environments/:id',
    {
      schema: {
        params: uuidParameterSchema,
        response: { 200: environmentResponseSchema },
      },
    },
    async (request, reply) => {
      const result = await environments.get(await requireActor(auth), request.params.id)
      reply.header('etag', `"${result.lockVersion}"`)
      return result
    },
  )
}
