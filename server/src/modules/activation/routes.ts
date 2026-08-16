import type { FastifyInstance } from 'fastify'

import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import { activationStatuses } from './model.js'
import type { ActivationService } from './model.js'

const nullableString = { anyOf: [{ type: 'string' }, { type: 'null' }] } as const
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
    status: { type: 'string', enum: activationStatuses },
    targetCount: { type: 'integer', minimum: 0 },
    activeCount: { type: 'integer', minimum: 0 },
    pendingCount: { type: 'integer', minimum: 0 },
    degradedCount: { type: 'integer', minimum: 0 },
    unknownCount: { type: 'integer', minimum: 0 },
    evaluatedAt: { anyOf: [{ type: 'string', format: 'date-time' }, { type: 'null' }] },
  },
} as const

export { activationSummarySchema }

export function registerActivationRoutes(
  app: FastifyInstance,
  auth: AuthService,
  activation: ActivationService,
): void {
  app.get<{
    Params: { releaseId: string }
    Querystring: { cursor?: string; limit?: string }
  }>(
    '/api/releases/:releaseId/activation',
    {
      schema: {
        params: {
          type: 'object',
          additionalProperties: false,
          required: ['releaseId'],
          properties: { releaseId: { type: 'string', format: 'uuid' } },
        },
        querystring: {
          type: 'object',
          additionalProperties: false,
          properties: {
            cursor: { type: 'string', format: 'uuid' },
            limit: { type: 'string', pattern: '^(?:[1-9]|[1-9][0-9]|100)$' },
          },
        },
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['releaseId', 'summary', 'items', 'nextCursor'],
            properties: {
              releaseId: { type: 'string', format: 'uuid' },
              summary: activationSummarySchema,
              items: {
                type: 'array',
                items: {
                  type: 'object',
                  additionalProperties: false,
                  required: [
                    'id',
                    'instanceKey',
                    'status',
                    'buildVersion',
                    'buildRevision',
                    'evidenceRevision',
                    'routeSnapshotGeneration',
                    'routeSnapshotFingerprintSha256',
                    'candidateStatus',
                    'candidateErrorCode',
                    'activeMd5',
                    'activeVersion',
                    'observedAt',
                    'expiresAt',
                  ],
                  properties: {
                    id: { type: 'string', format: 'uuid' },
                    instanceKey: { type: 'string' },
                    status: { type: 'string', enum: activationStatuses },
                    buildVersion: nullableString,
                    buildRevision: nullableString,
                    evidenceRevision: nullableString,
                    routeSnapshotGeneration: nullableString,
                    routeSnapshotFingerprintSha256: {
                      anyOf: [{ type: 'string', pattern: '^[0-9a-f]{64}$' }, { type: 'null' }],
                    },
                    candidateStatus: nullableString,
                    candidateErrorCode: nullableString,
                    activeMd5: {
                      anyOf: [{ type: 'string', pattern: '^[0-9a-f]{32}$' }, { type: 'null' }],
                    },
                    activeVersion: {
                      anyOf: [
                        {
                          type: 'string',
                          pattern: '^(?:0|[1-9][0-9]*)$',
                          maxLength: 20,
                        },
                        { type: 'null' },
                      ],
                    },
                    observedAt: {
                      anyOf: [{ type: 'string', format: 'date-time' }, { type: 'null' }],
                    },
                    expiresAt: {
                      anyOf: [{ type: 'string', format: 'date-time' }, { type: 'null' }],
                    },
                  },
                },
              },
              nextCursor: {
                anyOf: [{ type: 'string', format: 'uuid' }, { type: 'null' }],
              },
            },
          },
        },
      },
    },
    async (request) => {
      const rawLimit = request.query.limit ?? '50'
      const limit = Number(rawLimit)
      if (!Number.isInteger(limit) || limit < 1 || limit > 100) {
        throw badRequest('INVALID_PAGE_LIMIT', 'Page limit must be between 1 and 100')
      }
      return activation.listReleaseInstances(
        await requireActor(auth),
        request.params.releaseId,
        request.query.cursor ?? null,
        limit,
      )
    },
  )
}
