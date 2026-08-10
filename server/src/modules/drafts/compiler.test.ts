import assert from 'node:assert/strict'
import test from 'node:test'

import type { ProjectRoutesModel } from './model.js'
import { compileProjectRoutes } from './compiler.js'

function model(...sources: string[]): ProjectRoutesModel {
  return {
    schemaVersion: 2,
    kind: 'project_routes_yaml',
    routes: sources.map((source, index) => ({
      id: `00000000-0000-4000-8000-${String(index + 1).padStart(12, '0')}`,
      source,
    })),
  }
}

test('compiles independent YAML routes into ordered Java-compatible project JSON', () => {
  const result = compileProjectRoutes(
    'api.example.com',
    model(
      'path: /health\ntype: RESPONSE\nstatus: 200',
      'path: /api/*\ntype: PROXY\nservice: users/stable\ntimeout: 30s',
    ),
    17,
  )
  assert.deepEqual(result.issues, [])
  assert.ok(result.compiled)
  const payload: unknown = JSON.parse(result.compiled.payloadText)
  assert.deepEqual(payload, {
    host: { 'api.example.com': { https: 'S_NOT_MUST' } },
    routes: [
      { path: '/health', status: 200, type: 'RESPONSE' },
      { path: '/api/*', service: 'users/stable', timeout: '30s', type: 'PROXY' },
    ],
    version: 17,
  })
  assert.match(result.compiled.sha256, /^[0-9a-f]{64}$/u)
})

test('rejects unsafe YAML features and reports the owning route', () => {
  const routeId = '00000000-0000-4000-8000-000000000001'
  const result = compileProjectRoutes('api.example.com', {
    schemaVersion: 2,
    kind: 'project_routes_yaml',
    routes: [
      {
        id: routeId,
        source: 'path: &shared /one\ntype: RESPONSE\nrewrite: *shared',
      },
    ],
  })
  assert.equal(result.compiled, null)
  assert.ok(result.issues.every((issue) => issue.routeId === routeId))
  assert.ok(result.issues.some((issue) => issue.code === 'YAML_ANCHOR_NOT_ALLOWED'))
  assert.ok(result.issues.some((issue) => issue.code === 'YAML_ALIAS_NOT_ALLOWED'))
})

test('rejects duplicate, unknown, and structurally incomplete route fields', () => {
  const duplicate = compileProjectRoutes(
    'api.example.com',
    model('path: /one\npath: /two\ntype: RESPONSE'),
  )
  assert.ok(duplicate.issues.some((issue) => issue.code === 'DUPLICATE_KEY'))

  const unknown = compileProjectRoutes(
    'api.example.com',
    model('path: /one\ntype: RESPONSE\nfuture_field: true'),
  )
  assert.ok(unknown.issues.some((issue) => issue.code === 'UNKNOWN_ROUTE_FIELD'))

  const incomplete = compileProjectRoutes('api.example.com', model('status: 200'))
  assert.ok(incomplete.issues.some((issue) => issue.code === 'INVALID_ROUTE_PATH'))
  assert.ok(incomplete.issues.some((issue) => issue.code === 'INVALID_ROUTE_TYPE'))
})

test('comments and formatting do not change the compiled semantic digest', () => {
  const compact = compileProjectRoutes(
    'api.example.com',
    model('path: /health\ntype: RESPONSE\nstatus: 200'),
  )
  const commented = compileProjectRoutes(
    'api.example.com',
    model('# health endpoint\npath: /health\ntype: RESPONSE\nstatus: 200\n'),
  )
  assert.equal(compact.compiled?.sha256, commented.compiled?.sha256)
})
