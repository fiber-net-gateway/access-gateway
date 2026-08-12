import assert from 'node:assert/strict'
import test from 'node:test'

import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { CertificateRepository } from './repository.js'
import { DefaultCertificateService } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}
const environmentId = '00000000-0000-4000-8000-000000000003'

test('lists immutable versions only after resolving the logical certificate in the workspace', async () => {
  const versions = [
    {
      id: '00000000-0000-4000-8000-000000000005',
      version: 2,
      status: 'valid' as const,
      subject: 'CN=api.example.com',
      issuer: 'CN=Test CA',
      serialNumber: '02',
      fingerprintSha256: 'a'.repeat(64),
      dnsNames: ['api.example.com'],
      notBefore: '2026-01-01T00:00:00.000Z',
      notAfter: '2027-01-01T00:00:00.000Z',
      keyType: 'ec',
      createdAt: '2026-08-12T00:00:00.000Z',
    },
  ]
  const certificates = {
    findInEnvironment: async () => ({
      id: '00000000-0000-4000-8000-000000000004',
      internalId: '4',
      environmentInternalId: '3',
      lockVersion: '1',
      name: 'API certificate',
      currentVersion: versions[0],
      versionCount: 1,
      runtimeDeploymentStatus: 'activation_unknown',
      createdAt: '2026-01-01T00:00:00.000Z',
      updatedAt: '2026-08-12T00:00:00.000Z',
    }),
    listVersions: async () => versions,
  } as unknown as CertificateRepository
  const environments = {
    findWorkspace: async () => ({ id: environmentId }),
    internalIdForActor: async () => '3',
  } as unknown as EnvironmentRepository
  const service = new DefaultCertificateService(certificates, environments)

  assert.deepEqual(await service.listVersions(actor, '00000000-0000-4000-8000-000000000004'), {
    items: versions,
  })
})
