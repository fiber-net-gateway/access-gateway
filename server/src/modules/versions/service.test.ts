import assert from 'node:assert/strict'
import test from 'node:test'

import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { AppError } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import type { ProjectRoutesModel } from '../drafts/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import type { ConfigurationVersionDetail, SavedConfigurationVersion } from './model.js'
import { ConfigurationVersionRepository } from './repository.js'
import { DefaultConfigurationVersionService } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}
const projectId = '00000000-0000-4000-8000-000000000002'
const sourceVersionId = '00000000-0000-4000-8000-000000000003'
const currentVersionId = '00000000-0000-4000-8000-000000000004'
const routeId = '00000000-0000-4000-8000-000000000005'
const sourceModel: ProjectRoutesModel = {
  schemaVersion: 6,
  kind: 'project_routes_yaml',
  hostAliases: [],
  networkPolicy: {
    source: 'route',
    httpsRedirect: 'off',
    allowedCidrs: [],
    deniedCidrs: [],
  },
  routes: [
    {
      id: routeId,
      format: 'yaml',
      source: 'path: /source\ntype: RESPONSE\nstatus: 200',
    },
  ],
}
const sourceVersion: ConfigurationVersionDetail = {
  id: sourceVersionId,
  projectId,
  number: 12,
  relation: 'historical',
  baseVersionId: null,
  restoredFromVersionId: null,
  changeSummary: 'Historical source',
  routeCount: 1,
  modelSha256: '0'.repeat(64),
  validationState: 'valid',
  publicationStatus: 'never',
  createdBy: { id: actor.publicId, displayName: actor.displayName },
  createdAt: '2026-08-11T00:00:00.000Z',
  model: sourceModel,
}

function createService(capture: { savedInput?: Record<string, unknown> }) {
  const versions = {
    findDetail: async () => sourceVersion,
    save: async (input: Record<string, unknown>): Promise<SavedConfigurationVersion> => {
      capture.savedInput = input
      const model = input.model as ProjectRoutesModel
      return {
        version: {
          ...sourceVersion,
          id: '00000000-0000-4000-8000-000000000006',
          number: 19,
          relation: 'current',
          baseVersionId: currentVersionId,
          restoredFromVersionId: sourceVersionId,
          routeCount: model.routes.length,
          model,
        },
        lockVersion: '19',
      }
    },
  } as unknown as ConfigurationVersionRepository
  const projects = {
    findIdentity: async () => ({
      id: '2',
      public_id: Buffer.alloc(16, 2),
      environment_id: '3',
      environment_public_id: Buffer.alloc(16, 3),
      name: 'api.example.com',
    }),
  } as unknown as ProjectRepository
  const environments = {
    role: async () => 'maintainer',
  } as unknown as EnvironmentRepository
  return new DefaultConfigurationVersionService(
    versions,
    projects,
    environments,
    {} as NativeValidator,
  )
}

test('restoration saves an edited historical model while preserving its source', async () => {
  const capture: { savedInput?: Record<string, unknown> } = {}
  const service = createService(capture)
  const editedModel: ProjectRoutesModel = {
    ...sourceModel,
    routes: [
      {
        id: routeId,
        format: 'yaml',
        source: 'path: /edited\ntype: RESPONSE\nstatus: 201',
      },
    ],
  }

  const saved = await service.restore(
    actor,
    projectId,
    sourceVersionId,
    {
      lockVersion: '18',
      baseVersionId: currentVersionId,
      changeSummary: 'Edit from V12',
      forceSameContent: false,
      idempotencyKey: 'restore-edited-v19',
      model: editedModel,
    },
    'request-1',
  )

  assert.deepEqual(capture.savedInput?.model, editedModel)
  assert.equal(capture.savedInput?.restoredFromVersionId, sourceVersionId)
  assert.equal(capture.savedInput?.baseVersionId, currentVersionId)
  assert.deepEqual(saved.version.model, editedModel)
})

test('restoration without a model preserves exact historical restore behavior', async () => {
  const capture: { savedInput?: Record<string, unknown> } = {}
  const service = createService(capture)

  await service.restore(
    actor,
    projectId,
    sourceVersionId,
    {
      lockVersion: '18',
      baseVersionId: currentVersionId,
      changeSummary: 'Restore V12 exactly',
      forceSameContent: false,
      idempotencyKey: 'restore-exact-v19',
    },
    'request-2',
  )

  assert.deepEqual(capture.savedInput?.model, sourceModel)
  assert.equal(capture.savedInput?.restoredFromVersionId, sourceVersionId)
})

test('saving rejects invalid YAML before creating a configuration version', async () => {
  const capture: { savedInput?: Record<string, unknown> } = {}
  const service = createService(capture)

  await assert.rejects(
    service.save(
      actor,
      projectId,
      {
        lockVersion: '18',
        baseVersionId: currentVersionId,
        changeSummary: 'Broken YAML',
        forceSameContent: false,
        idempotencyKey: 'save-invalid-v19',
        model: {
          ...sourceModel,
          routes: [
            {
              id: routeId,
              format: 'yaml',
              source: 'path: /\nstatus: 200\ntype: RESPONSE\nresponse_headers:\n  X-Heassf',
            },
          ],
        },
      },
      'request-3',
    ),
    (error: unknown) =>
      error instanceof AppError &&
      error.code === 'INVALID_CONFIGURATION_YAML' &&
      error.fields[0]?.path === 'model.routes.0.source',
  )
  assert.equal(capture.savedInput, undefined)
})

test('edited historical restoration rejects invalid YAML before creating a version', async () => {
  const capture: { savedInput?: Record<string, unknown> } = {}
  const service = createService(capture)

  await assert.rejects(
    service.restore(
      actor,
      projectId,
      sourceVersionId,
      {
        lockVersion: '18',
        baseVersionId: currentVersionId,
        changeSummary: 'Broken historical edit',
        forceSameContent: false,
        idempotencyKey: 'restore-invalid-v19',
        model: {
          ...sourceModel,
          routes: [{ id: routeId, format: 'yaml', source: 'path: [' }],
        },
      },
      'request-4',
    ),
    (error: unknown) => error instanceof AppError && error.code === 'INVALID_CONFIGURATION_YAML',
  )
  assert.equal(capture.savedInput, undefined)
})
