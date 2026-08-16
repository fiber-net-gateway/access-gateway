import assert from 'node:assert/strict'
import test from 'node:test'

import type { NacosClient } from '../../integrations/nacos/model.js'
import { AppError } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from './repository.js'
import { ConfigurationVersionRepository } from '../versions/repository.js'
import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { fallbackAccessConfigLimits } from '../../integrations/native-validator/limits.js'
import type { BeginDecommissionReleaseInput } from '../releases/repository.js'
import { ReleaseRepository } from '../releases/repository.js'
import {
  compileDecommissionProjectList,
  DefaultReleaseService,
  validateProjectListTarget,
} from '../releases/service.js'
import { normalizeProjectDomain } from './service.js'

test('normalizes project domains to their lower-case ASCII form', () => {
  assert.equal(normalizeProjectDomain('  API.Example.COM.  '), 'api.example.com')
  assert.equal(normalizeProjectDomain('例子.测试'), 'xn--fsqu00a.xn--0zwm56d')
  assert.equal(normalizeProjectDomain('localhost'), 'localhost')
})

test('rejects values that are not exact DNS hostnames', () => {
  for (const value of [
    'https://example.com',
    '*.example.com',
    'example.com:443',
    '127.0.0.1',
    '-bad.example',
    'bad-.example',
    'a..example',
  ]) {
    assert.throws(
      () => normalizeProjectDomain(value),
      (error: unknown) => error instanceof AppError && error.code === 'INVALID_PROJECT_DOMAIN',
      value,
    )
  }
})

test('compiles a deterministic Project List target without rewriting an already absent domain', () => {
  assert.equal(
    compileDecommissionProjectList(
      'z.example.com;api.example.com;a.example.com',
      'api.example.com',
    ),
    'a.example.com;z.example.com',
  )
  assert.equal(
    compileDecommissionProjectList(' z.example.com ; a.example.com ', 'api.example.com'),
    ' z.example.com ; a.example.com ',
  )
  assert.equal(compileDecommissionProjectList(null, 'api.example.com'), '')
})

test('rejects Project List targets that exceed native entry and UTF-8 name limits', () => {
  assert.equal(
    validateProjectListTarget('a;b;', {
      ...fallbackAccessConfigLimits,
      projectList: { ...fallbackAccessConfigLimits.projectList, maxProjects: 2 },
    }),
    'a;b;',
  )
  assert.throws(
    () =>
      validateProjectListTarget('a;b;c', {
        ...fallbackAccessConfigLimits,
        projectList: { ...fallbackAccessConfigLimits.projectList, maxProjects: 2 },
      }),
    (error: unknown) => error instanceof AppError && error.code === 'PROJECT_LIST_LIMIT_EXCEEDED',
  )
  assert.throws(
    () =>
      validateProjectListTarget('返回', {
        ...fallbackAccessConfigLimits,
        projectList: { ...fallbackAccessConfigLimits.projectList, maxProjectNameBytes: 3 },
      }),
    (error: unknown) => error instanceof AppError && error.code === 'PROJECT_LIST_LIMIT_EXCEEDED',
  )
  assert.throws(
    () =>
      validateProjectListTarget('\u2003a', {
        ...fallbackAccessConfigLimits,
        projectList: { ...fallbackAccessConfigLimits.projectList, maxProjectNameBytes: 2 },
      }),
    (error: unknown) => error instanceof AppError && error.code === 'PROJECT_LIST_LIMIT_EXCEEDED',
  )
})

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'admin',
  displayName: 'Admin',
  platformAdmin: false,
}

function decommissionService(overrides: { role?: string; available?: boolean } = {}) {
  let prepared: BeginDecommissionReleaseInput | null = null
  let reads = 0
  const project = {
    id: '10',
    public_id: Buffer.alloc(16, 2),
    environment_id: '20',
    environment_public_id: Buffer.alloc(16, 3),
    name: 'api.example.com',
    status: 'active' as const,
  }
  const projects = {
    async findIdentityForHistory() {
      return project
    },
  } as unknown as ProjectRepository
  const environments = {
    async role() {
      return overrides.role ?? 'admin'
    },
    async findAccessibleByPublicId() {
      return {
        nacos: {
          endpoint: 'http://nacos:8848',
          namespace: 'public',
          tenant: '',
          credentialConfigured: false,
        },
        dataIds: {
          projects: 'ploto.unified-access.projects',
          routePrefix: 'ploto.unified-access.route.',
          routeGroup: 'ACCESS-SERVER',
          gray: 'ploto.unified-access.gray-match',
          grayGroup: 'DEFAULT_GROUP',
          namingGroup: 'DEFAULT_GROUP',
        },
      }
    },
  } as unknown as EnvironmentRepository
  const releaseView = {
    id: '00000000-0000-4000-8000-000000000010',
    sequence: '1',
    projectId: '00000000-0000-4000-8000-000000000002',
    kind: 'project_decommission' as const,
    title: '下线 api.example.com',
    description: 'retired',
    status: 'ready' as const,
    sourceConfigurationVersion: null,
    currentConfigurationVersionAtCreation: null,
    allocatedWireVersion: null,
    sourceModelSha256: 'a'.repeat(64),
    wireSha256: null,
    nativeValidator: null,
    compilerRevision: 'project-decommission-v1',
    validationErrors: [],
    resources: [],
    publication: { jobId: null, state: null },
    activationStatus: 'unknown' as const,
    activation: {
      status: 'unknown' as const,
      targetCount: 0,
      activeCount: 0,
      pendingCount: 0,
      degradedCount: 0,
      unknownCount: 0,
      evaluatedAt: null,
    },
    createdAt: '2026-08-13T00:00:00.000Z',
    publishedAt: null,
  }
  const releases = {
    async beginDecommission(input: BeginDecommissionReleaseInput) {
      prepared = input
      return { release: releaseView, replay: false }
    },
  } as unknown as ReleaseRepository
  const nacos = {
    available: overrides.available ?? true,
    async read() {
      reads += 1
      return {
        exists: true,
        content: 'z.example.com;api.example.com;a.example.com',
        sha256: 'b'.repeat(64),
        md5: 'c'.repeat(32),
      }
    },
  } as unknown as NacosClient
  const service = new DefaultReleaseService(
    releases,
    {} as ConfigurationVersionRepository,
    projects,
    environments,
    {
      available: true,
      contractVersion: 1,
      revision: 'test-validator',
      limits: fallbackAccessConfigLimits,
    } as NativeValidator,
    nacos,
  )
  return { service, prepared: () => prepared, reads: () => reads }
}

test('creates a decommission Release from a frozen Project List preflight', async () => {
  const { service, prepared, reads } = decommissionService()
  const release = await service.createDecommission(
    actor,
    '00000000-0000-4000-8000-000000000002',
    {
      confirmationDomain: 'api.example.com',
      reason: '  retired  ',
      expectedLockVersion: '4',
      idempotencyKey: 'decommission-1',
    },
    'request-1',
  )

  assert.equal(release.kind, 'project_decommission')
  assert.equal(reads(), 1)
  assert.equal(prepared()?.targetContent, 'a.example.com;z.example.com')
  assert.equal(prepared()?.reason, 'retired')
  assert.match(prepared()?.planText ?? '', /"kind":"project_decommission"/u)
})

test('rejects a mismatched confirmation and non-admin without reading Nacos', async () => {
  const mismatch = decommissionService()
  await assert.rejects(
    mismatch.service.createDecommission(
      actor,
      '00000000-0000-4000-8000-000000000002',
      {
        confirmationDomain: 'other.example.com',
        reason: 'retired',
        expectedLockVersion: '4',
        idempotencyKey: 'decommission-2',
      },
      'request-2',
    ),
    (error: unknown) => error instanceof AppError && error.code === 'PROJECT_CONFIRMATION_MISMATCH',
  )
  assert.equal(mismatch.reads(), 0)

  const maintainer = decommissionService({ role: 'maintainer' })
  await assert.rejects(
    maintainer.service.createDecommission(
      actor,
      '00000000-0000-4000-8000-000000000002',
      {
        confirmationDomain: 'api.example.com',
        reason: 'retired',
        expectedLockVersion: '4',
        idempotencyKey: 'decommission-3',
      },
      'request-3',
    ),
    (error: unknown) => error instanceof AppError && error.code === 'FORBIDDEN',
  )
  assert.equal(maintainer.reads(), 0)
})
