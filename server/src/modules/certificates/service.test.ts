import assert from 'node:assert/strict'
import test from 'node:test'

import { AppError } from '../../shared/errors.js'
import { publicIdToBuffer } from '../../shared/ids.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import { CertificateRepository } from './repository.js'
import { DefaultCertificateService } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}
const projectId = '00000000-0000-4000-8000-000000000002'
const environmentId = '00000000-0000-4000-8000-000000000003'
const certificateId = '00000000-0000-4000-8000-000000000004'

function serviceForDnsNames(dnsNames: readonly string[], capture: { bound?: boolean }) {
  const certificates = {
    findInEnvironment: async () => ({
      id: certificateId,
      internalId: '4',
      environmentInternalId: '3',
      name: 'Test certificate',
      status: 'valid',
      subject: 'CN=api.example.com',
      issuer: 'CN=Test CA',
      serialNumber: '01',
      fingerprintSha256: 'a'.repeat(64),
      dnsNames,
      notBefore: '2026-01-01T00:00:00.000Z',
      notAfter: '2027-01-01T00:00:00.000Z',
      keyType: 'ec',
      bindingCount: 0,
      runtimeDeploymentStatus: 'unsupported',
      createdAt: '2026-01-01T00:00:00.000Z',
    }),
    bind: async () => {
      capture.bound = true
    },
    getBinding: async () => ({
      projectId,
      domain: 'api.example.com',
      certificate: null,
      coverageStatus: 'unbound',
      runtimeDeploymentStatus: 'unsupported',
      boundAt: null,
    }),
  } as unknown as CertificateRepository
  const projects = {
    findIdentity: async () => ({
      id: '2',
      public_id: publicIdToBuffer(projectId),
      environment_id: '3',
      environment_public_id: publicIdToBuffer(environmentId),
      name: 'api.example.com',
    }),
  } as unknown as ProjectRepository
  const environments = {
    role: async () => 'maintainer',
  } as unknown as EnvironmentRepository
  return new DefaultCertificateService(certificates, projects, environments)
}

test('binds an exact or one-label wildcard SAN certificate to a covered Project', async () => {
  for (const dnsNames of [['api.example.com'], ['*.example.com']]) {
    const capture: { bound?: boolean } = {}
    await serviceForDnsNames(dnsNames, capture).bindProject(
      actor,
      projectId,
      { certificateId },
      'request-1',
    )
    assert.equal(capture.bound, true)
  }
})

test('fails closed before persistence when certificate SAN does not cover the Project', async () => {
  const capture: { bound?: boolean } = {}
  await assert.rejects(
    serviceForDnsNames(['other.example.com'], capture).bindProject(
      actor,
      projectId,
      { certificateId },
      'request-1',
    ),
    (error: unknown) =>
      error instanceof AppError && error.code === 'CERTIFICATE_DOMAIN_NOT_COVERED',
  )
  assert.equal(capture.bound, undefined)
})
