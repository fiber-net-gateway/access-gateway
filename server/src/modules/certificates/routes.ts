import type { FastifyInstance } from 'fastify'

import { badRequest } from '../../shared/errors.js'
import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import type { CreateCertificateInput, CreateCertificateVersionInput } from './model.js'
import type { CertificateService } from './service.js'

const certificateParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['certificateId'],
  properties: { certificateId: { type: 'string', format: 'uuid' } },
} as const

function parseIfMatch(value: string | undefined): string {
  if (!value)
    throw badRequest('IF_MATCH_REQUIRED', 'If-Match is required when updating a certificate')
  const match = /^"(0|[1-9][0-9]*)"$/u.exec(value)
  if (!match) {
    throw badRequest(
      'INVALID_IF_MATCH',
      'If-Match must contain the logical certificate lock version ETag',
    )
  }
  return match[1]!
}

const certificateVersionResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'version',
    'status',
    'subject',
    'issuer',
    'serialNumber',
    'fingerprintSha256',
    'dnsNames',
    'notBefore',
    'notAfter',
    'keyType',
    'createdAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    version: { type: 'integer', minimum: 1 },
    status: { type: 'string', enum: ['valid', 'expiring', 'expired', 'superseded'] },
    subject: { type: 'string' },
    issuer: { type: 'string' },
    serialNumber: { type: 'string' },
    fingerprintSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    dnsNames: { type: 'array', items: { type: 'string' } },
    notBefore: { type: 'string', format: 'date-time' },
    notAfter: { type: 'string', format: 'date-time' },
    keyType: { type: 'string' },
    createdAt: { type: 'string', format: 'date-time' },
  },
} as const

const certificateResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'name',
    'lockVersion',
    'currentVersion',
    'versionCount',
    'runtimeDeploymentStatus',
    'createdAt',
    'updatedAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    name: { type: 'string' },
    lockVersion: { type: 'string', pattern: '^(0|[1-9][0-9]*)$' },
    currentVersion: certificateVersionResponseSchema,
    versionCount: { type: 'integer', minimum: 1 },
    runtimeDeploymentStatus: { type: 'string', const: 'activation_unknown' },
    createdAt: { type: 'string', format: 'date-time' },
    updatedAt: { type: 'string', format: 'date-time' },
  },
} as const

const certificateMaterialSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['certificatePem', 'privateKeyPem'],
  properties: {
    certificatePem: { type: 'string', minLength: 1, maxLength: 1048576 },
    privateKeyPem: { type: 'string', minLength: 1, maxLength: 262144 },
  },
} as const

const certificateVersionMaterialSchema = {
  ...certificateMaterialSchema,
  properties: {
    ...certificateMaterialSchema.properties,
    confirmSniCoverageChange: { type: 'boolean' },
  },
} as const

export function registerCertificateRoutes(
  app: FastifyInstance,
  auth: AuthService,
  certificates: CertificateService,
): void {
  app.get(
    '/api/certificates',
    {
      schema: {
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: certificateResponseSchema } },
          },
        },
      },
    },
    async () => certificates.list(await requireActor(auth)),
  )

  app.post<{ Body: CreateCertificateInput }>(
    '/api/certificates',
    {
      schema: {
        body: {
          ...certificateMaterialSchema,
          required: ['name', 'certificatePem', 'privateKeyPem'],
          properties: {
            name: { type: 'string', minLength: 1, maxLength: 255 },
            ...certificateMaterialSchema.properties,
          },
        },
        response: { 201: certificateResponseSchema },
      },
    },
    async (request, reply) =>
      reply
        .code(201)
        .send(
          await certificates.create(await requireActor(auth), request.body, String(request.id)),
        ),
  )

  app.get<{ Params: { certificateId: string } }>(
    '/api/certificates/:certificateId/versions',
    {
      schema: {
        params: certificateParameters,
        response: {
          200: {
            type: 'object',
            additionalProperties: false,
            required: ['items'],
            properties: { items: { type: 'array', items: certificateVersionResponseSchema } },
          },
        },
      },
    },
    async (request) =>
      certificates.listVersions(await requireActor(auth), request.params.certificateId),
  )

  app.post<{
    Params: { certificateId: string }
    Body: CreateCertificateVersionInput
    Headers: { 'if-match'?: string }
  }>(
    '/api/certificates/:certificateId/versions',
    {
      schema: {
        params: certificateParameters,
        headers: {
          type: 'object',
          required: ['if-match'],
          properties: { 'if-match': { type: 'string' } },
        },
        body: certificateVersionMaterialSchema,
        response: { 201: certificateResponseSchema },
      },
    },
    async (request, reply) =>
      reply
        .code(201)
        .send(
          await certificates.createVersion(
            await requireActor(auth),
            request.params.certificateId,
            request.body,
            parseIfMatch(request.headers['if-match']),
            String(request.id),
          ),
        ),
  )
}
