import assert from 'node:assert/strict'
import test from 'node:test'

import { AppError } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import type { EnvironmentView } from './model.js'
import { EnvironmentRepository } from './repository.js'
import { DefaultEnvironmentService } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Administrator',
  platformAdmin: true,
}

const workspace: EnvironmentView = {
  id: '00000000-0000-4000-8000-000000000002',
  code: 'demo',
  name: 'Demo',
  tier: 'local',
  status: 'active',
  nacos: {
    endpoint: 'http://localhost:8848',
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
  zone: '',
  protectionPolicy: {},
  lockVersion: '0',
  createdAt: '2026-08-10T00:00:00.000000Z',
  updatedAt: '2026-08-10T00:00:00.000000Z',
}

test('fixed workspace returns the repository-selected environment', async () => {
  const repository = {
    findWorkspace: async () => workspace,
  } as unknown as EnvironmentRepository
  const service = new DefaultEnvironmentService(repository)

  assert.deepEqual(await service.getWorkspace(actor), workspace)
})

test('fixed workspace remains unavailable until bootstrap creates it', async () => {
  const repository = {
    findWorkspace: async () => null,
  } as unknown as EnvironmentRepository
  const service = new DefaultEnvironmentService(repository)

  await assert.rejects(
    service.getWorkspace(actor),
    (error: unknown) => error instanceof AppError && error.code === 'NOT_FOUND',
  )
})
