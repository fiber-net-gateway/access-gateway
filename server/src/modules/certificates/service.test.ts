import assert from 'node:assert/strict'
import test from 'node:test'

import type { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { publicIdToBuffer } from '../../shared/ids.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository, type ProjectIdentityRow } from '../projects/repository.js'
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

test('repository projects the match kind used by automatic resolution', async () => {
  let executedSql = ''
  const certificateId = '00000000-0000-4000-8000-000000000004'
  const versionId = '00000000-0000-4000-8000-000000000005'
  const pool = {
    execute: async (sql: string) => {
      executedSql = sql
      return [
        [
          {
            series_internal_id: '4',
            series_public_id: publicIdToBuffer(certificateId),
            environment_id: '3',
            display_name: 'API certificate',
            managed_dns_names_json: ['api.example.com'],
            lock_version: '0',
            series_created_at: '2026-08-12 00:00:00.000000',
            series_updated_at: '2026-08-12 00:00:00.000000',
            version_internal_id: '5',
            version_public_id: publicIdToBuffer(versionId),
            version_no: 1,
            lifecycle_state: 'active',
            fingerprint_sha256: Buffer.alloc(32, 1),
            serial_number: '01',
            subject: 'CN=api.example.com',
            issuer: 'CN=Test CA',
            dns_names_json: ['api.example.com'],
            not_before: '2026-08-12 00:00:00.000000',
            not_after: '2027-08-12 00:00:00.000000',
            key_type: 'ec',
            version_created_at: '2026-08-12 00:00:00.000000',
            version_count: '1',
            matched_project_count: '1',
            match_kind: 'exact',
          },
        ],
        [],
      ]
    },
  } as unknown as DatabasePool
  const repository = new CertificateRepository(pool, {} as DocumentRepository)

  const resolution = await repository.resolveProject({
    id: '2',
    public_id: publicIdToBuffer(projectId),
    environment_id: '3',
    environment_public_id: publicIdToBuffer(environmentId),
    name: 'api.example.com',
  } as ProjectIdentityRow)

  assert.match(executedSql, /matched_name\.match_kind AS match_kind/u)
  assert.equal(resolution.resolutionStatus, 'matched')
  assert.equal(resolution.certificate?.id, certificateId)
})

test('returns the repository automatic certificate resolution without creating a binding', async () => {
  const expected = {
    projectId,
    domain: 'api.example.com',
    resolutionStatus: 'uncovered' as const,
    certificate: null,
    matches: [],
    runtimeDeploymentStatus: 'unsupported' as const,
  }
  let resolved = false
  const certificates = {
    resolveProject: async () => {
      resolved = true
      return expected
    },
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
  const environments = {} as EnvironmentRepository
  const service = new DefaultCertificateService(certificates, projects, environments)

  assert.deepEqual(await service.resolveProject(actor, projectId), expected)
  assert.equal(resolved, true)
})

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
      managedDnsNames: ['api.example.com'],
      currentVersion: versions[0],
      versionCount: 1,
      matchedProjectCount: 1,
      runtimeDeploymentStatus: 'unsupported',
      createdAt: '2026-01-01T00:00:00.000Z',
      updatedAt: '2026-08-12T00:00:00.000Z',
    }),
    listVersions: async () => versions,
  } as unknown as CertificateRepository
  const projects = {} as ProjectRepository
  const environments = {
    findWorkspace: async () => ({ id: environmentId }),
    internalIdForActor: async () => '3',
  } as unknown as EnvironmentRepository
  const service = new DefaultCertificateService(certificates, projects, environments)

  assert.deepEqual(await service.listVersions(actor, '00000000-0000-4000-8000-000000000004'), {
    items: versions,
  })
})
