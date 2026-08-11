import assert from 'node:assert/strict'
import test from 'node:test'

import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { AppError } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import { DraftRepository } from './repository.js'
import { DefaultDraftService } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}

test('legacy draft revision API rejects invalid YAML before persistence', async () => {
  let persisted = false
  const drafts = {
    findByPublicId: async () => ({
      id: '00000000-0000-4000-8000-000000000002',
      projectId: '00000000-0000-4000-8000-000000000003',
    }),
    createRevision: async () => {
      persisted = true
      throw new Error('Unexpected persistence')
    },
  } as unknown as DraftRepository
  const projects = {
    findIdentity: async () => ({
      id: '3',
      public_id: Buffer.alloc(16, 3),
      environment_id: '4',
      environment_public_id: Buffer.alloc(16, 4),
      name: 'api.example.com',
    }),
  } as unknown as ProjectRepository
  const environments = {
    role: async () => 'maintainer',
  } as unknown as EnvironmentRepository
  const service = new DefaultDraftService(drafts, projects, environments, {} as NativeValidator)

  await assert.rejects(
    service.createRevision(
      actor,
      '00000000-0000-4000-8000-000000000002',
      {
        lockVersion: '1',
        changeSummary: 'Broken YAML',
        model: {
          schemaVersion: 2,
          kind: 'project_routes_yaml',
          routes: [{ id: '00000000-0000-4000-8000-000000000004', source: 'path: [' }],
        },
      },
      'request-1',
    ),
    (error: unknown) => error instanceof AppError && error.code === 'INVALID_CONFIGURATION_YAML',
  )
  assert.equal(persisted, false)
})
