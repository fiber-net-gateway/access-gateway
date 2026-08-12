import assert from 'node:assert/strict'
import test from 'node:test'

import type { ProjectRoutesModel } from './model.js'
import { compileProjectRoutes } from './compiler.js'

function model(...sources: string[]): ProjectRoutesModel {
  return {
    schemaVersion: 3,
    kind: 'project_routes_yaml',
    networkPolicy: { source: 'route', allowedCidrs: [], deniedCidrs: [] },
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
    schemaVersion: 3,
    kind: 'project_routes_yaml',
    networkPolicy: { source: 'route', allowedCidrs: [], deniedCidrs: [] },
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

test('rejects scalar header blocks that YAML accepts but the native route codec cannot consume', () => {
  const result = compileProjectRoutes(
    'api.example.com',
    model('path: /\nstatus: 200\ntype: RESPONSE\nresponse_headers:\n  X-Heassf'),
  )

  assert.equal(result.compiled, null)
  assert.ok(
    result.issues.some(
      (issue) => issue.code === 'INVALID_ROUTE_FIELD_TYPE' && issue.path === 'response_headers',
    ),
  )
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

test('injects an authoritative Project network policy into every route', () => {
  const candidate = model(
    'path: /health\ntype: RESPONSE\nstatus: 200',
    'path: /api/*\ntype: PROXY\nservice: users',
  )
  const result = compileProjectRoutes('api.example.com', {
    ...candidate,
    networkPolicy: {
      source: 'project',
      allowedCidrs: ['10.0.0.0/8', '2001:db8::/32'],
      deniedCidrs: ['10.1.0.0/16'],
    },
  })

  assert.deepEqual(result.issues, [])
  const payload = JSON.parse(result.compiled!.payloadText) as {
    routes: Array<{ allows: string[] }>
  }
  assert.deepEqual(payload.routes[0]?.allows, ['10.0.0.0/8', '2001:db8::/32', '!10.1.0.0/16'])
  assert.deepEqual(payload.routes[1]?.allows, payload.routes[0]?.allows)
})

test('rejects malformed Project CIDRs and Route overrides under authoritative policy', () => {
  const candidate = model('path: /\ntype: RESPONSE\nstatus: 200\nallows: []')
  const result = compileProjectRoutes('api.example.com', {
    ...candidate,
    networkPolicy: {
      source: 'project',
      allowedCidrs: ['10.0.0.0/99'],
      deniedCidrs: [],
    },
  })

  assert.equal(result.compiled, null)
  assert.ok(result.issues.some((issue) => issue.code === 'INVALID_NETWORK_POLICY_CIDR'))
  assert.ok(result.issues.some((issue) => issue.code === 'ROUTE_NETWORK_POLICY_CONFLICT'))
})
