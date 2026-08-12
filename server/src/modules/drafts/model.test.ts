import assert from 'node:assert/strict'
import test from 'node:test'

import { normalizeStoredProjectRoutesModel } from './model.js'

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
  assert.equal(first?.schemaVersion, 3)
  assert.deepEqual(first?.networkPolicy, {
    source: 'route',
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

test('upgrades schema v2 YAML models with Route-owned network policy semantics', () => {
  const upgraded = normalizeStoredProjectRoutesModel({
    schemaVersion: 2,
    kind: 'project_routes_yaml',
    routes: [],
  })

  assert.deepEqual(upgraded, {
    schemaVersion: 3,
    kind: 'project_routes_yaml',
    networkPolicy: { source: 'route', allowedCidrs: [], deniedCidrs: [] },
    routes: [],
  })
})
