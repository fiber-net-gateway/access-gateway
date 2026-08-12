import type { FastifyInstance } from 'fastify'

import { requireActor } from '../auth/http.js'
import type { AuthService } from '../auth/model.js'
import type { BindProjectCertificateInput, CreateCertificateInput } from './model.js'
import type { CertificateService } from './service.js'

const projectParameters = {
  type: 'object',
  additionalProperties: false,
  required: ['projectId'],
  properties: { projectId: { type: 'string', format: 'uuid' } },
} as const

const certificateResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'id',
    'name',
    'status',
    'subject',
    'issuer',
    'serialNumber',
    'fingerprintSha256',
    'dnsNames',
    'notBefore',
    'notAfter',
    'keyType',
    'bindingCount',
    'runtimeDeploymentStatus',
    'createdAt',
  ],
  properties: {
    id: { type: 'string', format: 'uuid' },
    name: { type: 'string' },
    status: { type: 'string', enum: ['valid', 'expiring', 'expired', 'superseded'] },
    subject: { type: 'string' },
    issuer: { type: 'string' },
    serialNumber: { type: 'string' },
    fingerprintSha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
    dnsNames: { type: 'array', items: { type: 'string' } },
    notBefore: { type: 'string', format: 'date-time' },
    notAfter: { type: 'string', format: 'date-time' },
    keyType: { type: 'string' },
    bindingCount: { type: 'integer', minimum: 0 },
    runtimeDeploymentStatus: { type: 'string', const: 'unsupported' },
    createdAt: { type: 'string', format: 'date-time' },
  },
} as const

const bindingResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: [
    'projectId',
    'domain',
    'certificate',
    'coverageStatus',
    'runtimeDeploymentStatus',
    'boundAt',
  ],
  properties: {
    projectId: { type: 'string', format: 'uuid' },
    domain: { type: 'string' },
    certificate: { anyOf: [{ type: 'null' }, certificateResponseSchema] },
    coverageStatus: { type: 'string', enum: ['covered', 'unbound'] },
    runtimeDeploymentStatus: { type: 'string', const: 'unsupported' },
    boundAt: { anyOf: [{ type: 'null' }, { type: 'string', format: 'date-time' }] },
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
          type: 'object',
          additionalProperties: false,
          required: ['name', 'certificatePem', 'privateKeyPem'],
          properties: {
            name: { type: 'string', minLength: 1, maxLength: 255 },
            certificatePem: { type: 'string', minLength: 1, maxLength: 1048576 },
            privateKeyPem: { type: 'string', minLength: 1, maxLength: 262144 },
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

  app.get<{ Params: { projectId: string } }>(
    '/api/projects/:projectId/certificate',
    { schema: { params: projectParameters, response: { 200: bindingResponseSchema } } },
    async (request) =>
      certificates.getProjectBinding(await requireActor(auth), request.params.projectId),
  )

  app.put<{ Params: { projectId: string }; Body: BindProjectCertificateInput }>(
    '/api/projects/:projectId/certificate',
    {
      schema: {
        params: projectParameters,
        body: {
          type: 'object',
          additionalProperties: false,
          required: ['certificateId'],
          properties: { certificateId: { type: 'string', format: 'uuid' } },
        },
        response: { 200: bindingResponseSchema },
      },
    },
    async (request) =>
      certificates.bindProject(
        await requireActor(auth),
        request.params.projectId,
        request.body,
        String(request.id),
      ),
  )

  app.delete<{ Params: { projectId: string } }>(
    '/api/projects/:projectId/certificate',
    { schema: { params: projectParameters, response: { 200: bindingResponseSchema } } },
    async (request) =>
      certificates.unbindProject(
        await requireActor(auth),
        request.params.projectId,
        String(request.id),
      ),
  )
}
