import type { FastifyInstance } from 'fastify'

import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import type { TlsSniService } from './service.js'

const certificateSummarySchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'name',
    'version',
    'status',
    'notAfter',
    'fingerprintSha256',
    'runtimeDeploymentStatus',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    name: { type: 'string' },
    version: { type: 'integer', minimum: 1 },
    status: { type: 'string', enum: ['valid', 'expiring', 'expired', 'superseded'] },
    notAfter: { type: 'string', format: 'date-time' },
    fingerprintSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    runtimeDeploymentStatus: { type: 'string', const: 'unsupported' },
  },
} as const

const resolutionResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'serverName',
    'resolutionStatus',
    'matchKind',
    'certificate',
    'matches',
    'runtimeDeploymentStatus',
  ],
  properties: {
    serverName: { type: 'string' },
    resolutionStatus: { type: 'string', enum: ['matched', 'uncovered', 'conflict'] },
    matchKind: {
      anyOf: [{ type: 'null' }, { type: 'string', enum: ['exact', 'wildcard'] }],
    },
    certificate: { anyOf: [{ type: 'null' }, certificateSummarySchema] },
    matches: { type: 'array', items: certificateSummarySchema },
    runtimeDeploymentStatus: { type: 'string', const: 'unsupported' },
  },
} as const

export function registerTlsSniRoutes(
  app: FastifyInstance,
  auth: AuthService,
  tlsSni: TlsSniService,
): void {
  app.get<{ Querystring: { serverName: string } }>(
    '/api/tls/sni-resolution',
    {
      schema: {
        querystring: {
          type: 'object',
          additionalProperties: false,
          required: ['serverName'],
          properties: { serverName: { type: 'string', minLength: 1, maxLength: 255 } },
        },
        response: { 200: resolutionResponseSchema },
      },
    },
    async (request) => tlsSni.resolve(await requireActor(auth), request.query.serverName),
  )
}
