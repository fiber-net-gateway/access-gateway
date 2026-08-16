import type { FastifyInstance } from 'fastify'

import type { SystemStatusService } from './service.js'

const capabilitySchema = {
  type: 'object',
  additionalProperties: false,
  required: ['status', 'detail'],
  properties: {
    status: { type: 'string', enum: ['ready', 'unconfigured', 'unavailable'] },
    detail: { type: 'string' },
  },
} as const

const positiveIntegerSchema = { type: 'integer', minimum: 1 } as const

const accessConfigLimitsSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['schemaVersion', 'projectList', 'projectRoute', 'grayRules'],
  properties: {
    schemaVersion: { type: 'integer', const: 1 },
    projectList: {
      type: 'object',
      additionalProperties: false,
      required: ['maxPayloadBytes', 'maxProjects', 'maxProjectNameBytes'],
      properties: {
        maxPayloadBytes: positiveIntegerSchema,
        maxProjects: positiveIntegerSchema,
        maxProjectNameBytes: positiveIntegerSchema,
      },
    },
    projectRoute: {
      type: 'object',
      additionalProperties: false,
      required: [
        'maxPayloadBytes',
        'maxHosts',
        'maxRoutes',
        'maxHostPatternBytes',
        'maxPathBytes',
        'maxMethodBytes',
        'maxServiceBytes',
        'maxClusterBytes',
        'maxConditionBytes',
        'maxScriptBytes',
        'maxTemplateBytes',
        'maxHeaderEntries',
        'maxHeaderNameBytes',
        'maxHeaderValueBytes',
        'maxCidrsPerRoute',
        'maxCidrBytes',
        'maxAddressesPerRoute',
        'maxAddressBytes',
        'maxStaticResponseBodyBytes',
        'maxStaticResponseBytes',
        'maxPathVariables',
        'maxTemplateExpressions',
        'maxCompiledPrograms',
        'maxEstimatedSnapshotBytes',
      ],
      properties: {
        maxPayloadBytes: positiveIntegerSchema,
        maxHosts: positiveIntegerSchema,
        maxRoutes: positiveIntegerSchema,
        maxHostPatternBytes: positiveIntegerSchema,
        maxPathBytes: positiveIntegerSchema,
        maxMethodBytes: positiveIntegerSchema,
        maxServiceBytes: positiveIntegerSchema,
        maxClusterBytes: positiveIntegerSchema,
        maxConditionBytes: positiveIntegerSchema,
        maxScriptBytes: positiveIntegerSchema,
        maxTemplateBytes: positiveIntegerSchema,
        maxHeaderEntries: positiveIntegerSchema,
        maxHeaderNameBytes: positiveIntegerSchema,
        maxHeaderValueBytes: positiveIntegerSchema,
        maxCidrsPerRoute: positiveIntegerSchema,
        maxCidrBytes: positiveIntegerSchema,
        maxAddressesPerRoute: positiveIntegerSchema,
        maxAddressBytes: positiveIntegerSchema,
        maxStaticResponseBodyBytes: positiveIntegerSchema,
        maxStaticResponseBytes: positiveIntegerSchema,
        maxPathVariables: positiveIntegerSchema,
        maxTemplateExpressions: positiveIntegerSchema,
        maxCompiledPrograms: positiveIntegerSchema,
        maxEstimatedSnapshotBytes: positiveIntegerSchema,
      },
    },
    grayRules: {
      type: 'object',
      additionalProperties: false,
      required: ['maxPayloadBytes', 'maxRules', 'maxEntryBytes', 'maxCidrsPerRule', 'maxCidrBytes'],
      properties: {
        maxPayloadBytes: positiveIntegerSchema,
        maxRules: positiveIntegerSchema,
        maxEntryBytes: positiveIntegerSchema,
        maxCidrsPerRule: positiveIntegerSchema,
        maxCidrBytes: positiveIntegerSchema,
      },
    },
  },
} as const

const systemStatusSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['status', 'service', 'dependencies'],
  properties: {
    status: { type: 'string', enum: ['ready', 'degraded'] },
    service: { type: 'string', const: 'access-gateway-console-api' },
    dependencies: {
      type: 'object',
      additionalProperties: false,
      required: [
        'database',
        'schema',
        'authentication',
        'nativeValidator',
        'publicationWorker',
        'activationCollector',
      ],
      properties: {
        database: capabilitySchema,
        schema: capabilitySchema,
        authentication: capabilitySchema,
        nativeValidator: {
          ...capabilitySchema,
          required: ['status', 'detail', 'contractVersion', 'revision', 'limits'],
          properties: {
            ...capabilitySchema.properties,
            contractVersion: { type: 'integer', minimum: 1 },
            revision: { anyOf: [{ type: 'string' }, { type: 'null' }] },
            limits: { anyOf: [accessConfigLimitsSchema, { type: 'null' }] },
          },
        },
        publicationWorker: capabilitySchema,
        activationCollector: capabilitySchema,
      },
    },
  },
} as const

export function registerSystemRoutes(app: FastifyInstance, system: SystemStatusService): void {
  app.get('/api/system/status', { schema: { response: { 200: systemStatusSchema } } }, async () =>
    system.get(),
  )
  app.get(
    '/api/health/live',
    {
      schema: {
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['status'],
            properties: { status: { type: 'string', const: 'ok' } },
          },
        },
      },
    },
    async () => ({ status: 'ok' as const }),
  )
  app.get(
    '/api/health/ready',
    { schema: { response: { 200: systemStatusSchema, 503: systemStatusSchema } } },
    async (_request, reply) => {
      const status = await system.get()
      return reply.code(status.status === 'ready' ? 200 : 503).send(status)
    },
  )
}
