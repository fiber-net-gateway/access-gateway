import assert from 'node:assert/strict'
import test from 'node:test'

import { fallbackAccessConfigLimits } from '../../integrations/native-validator/limits.js'
import { isProjectRoutesModel, normalizeStoredProjectRoutesModel } from './model.js'

test('upgrades legacy whole-project JSON revisions to stable YAML route items', () => {
  const legacy = {
    schemaVersion: 1,
    kind: 'project_route',
    hosts: [{ pattern: 'api.example.com' }],
    routes: [{ path: '/health', type: 'RESPONSE', status: 200 }],
  }
  const first = normalizeStoredProjectRoutesModel(legacy)
  const second = normalizeStoredProjectRoutesModel(legacy)
  assert.deepEqual(first, second)
  assert.equal(first?.kind, 'project_routes_yaml')
  assert.equal(first?.schemaVersion, 5)
  assert.equal(first?.routes[0]?.format, 'yaml')
  assert.deepEqual(first?.networkPolicy, {
    source: 'route',
    httpsRedirect: 'off',
    allowedCidrs: [],
    deniedCidrs: [],
  })
  assert.match(first?.routes[0]?.source ?? '', /path: \/health/u)
  assert.match(first?.routes[0]?.id ?? '', /^[0-9a-f-]{36}$/u)
})

test('rejects duplicate route IDs in persisted YAML models', () => {
  const id = '00000000-0000-4000-8000-000000000001'
  assert.equal(
    normalizeStoredProjectRoutesModel({
      schemaVersion: 2,
      kind: 'project_routes_yaml',
      routes: [
        { id, source: 'path: /one' },
        { id, source: 'path: /two' },
      ],
    }),
    null,
  )
})

test('upgrades schema v2 YAML models with safe network policy defaults', () => {
  const upgraded = normalizeStoredProjectRoutesModel({
    schemaVersion: 2,
    kind: 'project_routes_yaml',
    routes: [],
  })

  assert.deepEqual(upgraded, {
    schemaVersion: 5,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
    routes: [],
  })
})

test('upgrades schema v3 network policies with HTTPS redirect disabled', () => {
  const upgraded = normalizeStoredProjectRoutesModel({
    schemaVersion: 3,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'project',
      allowedCidrs: ['10.0.0.0/8'],
      deniedCidrs: ['10.1.0.0/16'],
    },
    routes: [],
  })

  assert.deepEqual(upgraded, {
    schemaVersion: 5,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'project',
      httpsRedirect: 'off',
      allowedCidrs: ['10.0.0.0/8'],
      deniedCidrs: ['10.1.0.0/16'],
    },
    routes: [],
  })
})

test('upgrades schema v4 YAML items with an explicit format discriminator', () => {
  const upgraded = normalizeStoredProjectRoutesModel({
    schemaVersion: 4,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
    routes: [{ id: '00000000-0000-4000-8000-000000000001', source: 'path: /' }],
  })

  assert.equal(upgraded?.schemaVersion, 5)
  assert.deepEqual(upgraded?.routes[0], {
    id: '00000000-0000-4000-8000-000000000001',
    format: 'yaml',
    source: 'path: /',
  })
})

test('checks mixed route source limits in UTF-8 bytes', () => {
  const limits = {
    ...fallbackAccessConfigLimits,
    projectRoute: {
      ...fallbackAccessConfigLimits.projectRoute,
      maxScriptBytes: 3,
      maxPayloadBytes: 32,
    },
  }
  assert.equal(
    isProjectRoutesModel(
      {
        schemaVersion: 5,
        kind: 'project_routes_yaml',
        networkPolicy: {
          source: 'route',
          httpsRedirect: 'off',
          allowedCidrs: [],
          deniedCidrs: [],
        },
        routes: [
          {
            id: '00000000-0000-4000-8000-000000000001',
            format: 'js',
            path: '/',
            source: '返回',
          },
        ],
      },
      limits,
    ),
    false,
  )
})

test('rejects oversized legacy route collections before normalization', () => {
  assert.equal(
    normalizeStoredProjectRoutesModel({
      schemaVersion: 1,
      kind: 'project_route',
      hosts: [],
      routes: Array.from({ length: fallbackAccessConfigLimits.projectRoute.maxRoutes + 1 }, () => ({
        path: '/',
      })),
    }),
    null,
  )
})
