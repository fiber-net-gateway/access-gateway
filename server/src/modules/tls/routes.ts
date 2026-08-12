import type { FastifyInstance } from 'fastify'

import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import type { TlsCertificateReleaseService } from './release-service.js'
import type { TlsSniService } from './service.js'

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
    runtimeDeploymentStatus: { type: 'string', const: 'activation_unknown' },
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
    runtimeDeploymentStatus: { type: 'string', const: 'activation_unknown' },
  },
} as const

export function registerTlsSniRoutes(
  app: FastifyInstance,
  auth: AuthService,
  tlsSni: TlsSniService,
  tlsReleases: TlsCertificateReleaseService,
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

  const releaseResourceSchema = {
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
      verifiedSha256: {
        anyOf: [{ type: 'string', pattern: '^[0-9a-f]{64}$' }, { type: 'null' }],
      },
      verifiedAt: { anyOf: [{ type: 'string', format: 'date-time' }, { type: 'null' }] },
    },
  } as const
  const tlsReleaseSchema = {
    type: 'object',
    additionalProperties: false,
    required: [
      'id',
      'sequence',
      'status',
      'defaultCertificateId',
      'certificateCount',
      'wireSha256',
      'resource',
      'publication',
      'activationStatus',
      'createdAt',
      'publishedAt',
    ],
    properties: {
      id: { type: 'string', format: 'uuid' },
      sequence: { type: 'string', pattern: '^[1-9][0-9]*$' },
      status: { type: 'string' },
      defaultCertificateId: { type: 'string', format: 'uuid' },
      certificateCount: { type: 'integer', minimum: 1, maximum: 128 },
      wireSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
      resource: releaseResourceSchema,
      publication: {
        type: 'object',
        additionalProperties: false,
        required: ['jobId', 'state'],
        properties: {
          jobId: { anyOf: [{ type: 'string', format: 'uuid' }, { type: 'null' }] },
          state: { anyOf: [{ type: 'string' }, { type: 'null' }] },
        },
      },
      activationStatus: { type: 'string', const: 'unknown' },
      createdAt: { type: 'string', format: 'date-time' },
      publishedAt: { anyOf: [{ type: 'string', format: 'date-time' }, { type: 'null' }] },
    },
  } as const

  app.get(
    '/api/tls/releases',
    {
      schema: {
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: tlsReleaseSchema } },
          },
        },
      },
    },
    async () => tlsReleases.list(await requireActor(auth)),
  )

  app.get<{ Params: { releaseId: string } }>(
    '/api/tls/releases/:releaseId',
    {
      schema: {
        params: {
          type: 'object',
          additionalProperties: false,
          required: ['releaseId'],
          properties: { releaseId: { type: 'string', format: 'uuid' } },
        },
        response: { 200: tlsReleaseSchema },
      },
    },
    async (request) => tlsReleases.get(await requireActor(auth), request.params.releaseId),
  )

  app.post<{
    Headers: { 'idempotency-key'?: string }
    Body: { defaultCertificateId: string }
  }>(
    '/api/tls/releases',
    {
      schema: {
        headers: {
          type: 'object',
          required: ['idempotency-key'],
          properties: { 'idempotency-key': { type: 'string', minLength: 1, maxLength: 128 } },
        },
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['defaultCertificateId'],
          properties: { defaultCertificateId: { type: 'string', format: 'uuid' } },
        },
        response: { 201: tlsReleaseSchema },
      },
    },
    async (request, reply) => {
      const release = await tlsReleases.create(
        await requireActor(auth),
        {
          defaultCertificateId: request.body.defaultCertificateId,
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
    '/api/tls/releases/:releaseId/publications',
    {
      schema: {
        params: {
          type: 'object',
          additionalProperties: false,
          required: ['releaseId'],
          properties: { releaseId: { type: 'string', format: 'uuid' } },
        },
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
              release: tlsReleaseSchema,
            },
          },
        },
      },
    },
    async (request, reply) => {
      const queued = await tlsReleases.queuePublication(
        await requireActor(auth),
        request.params.releaseId,
        parseIdempotencyKey(request.headers['idempotency-key']),
        String(request.id),
      )
      return reply.code(202).send(queued)
    },
  )
}
