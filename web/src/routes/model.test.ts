import assert from 'node:assert/strict'
import test from 'node:test'

import {
  analyzeRouteSource,
  createRouteItem,
  duplicateRouteItem,
  initialRouteModel,
} from './model.js'

test('creates an empty ordered YAML route model', () => {
  assert.deepEqual(initialRouteModel(), {
    schemaVersion: 2,
    kind: 'project_routes_yaml',
    routes: [],
  })
})

test('creates independent RESPONSE and PROXY YAML route items', () => {
  const response = createRouteItem('RESPONSE')
  const proxy = createRouteItem('PROXY')
  assert.notEqual(response.id, proxy.id)
  assert.equal(analyzeRouteSource(response).type, 'RESPONSE')
  assert.equal(analyzeRouteSource(proxy).type, 'PROXY')

  const duplicate = duplicateRouteItem(proxy)
  assert.notEqual(duplicate.id, proxy.id)
  assert.equal(duplicate.source, proxy.source)
})

test('reports malformed documents, duplicate keys, anchors, and non-mapping roots', () => {
  for (const source of ['path: [', 'path: /one\npath: /two\ntype: RESPONSE', '- one\n- two']) {
    const result = analyzeRouteSource({ id: crypto.randomUUID(), source })
    assert.ok(result.issues.length > 0, source)
  }

  const anchored = analyzeRouteSource({
    id: crypto.randomUUID(),
    source: 'path: &routePath /one\ntype: RESPONSE\nrewrite: *routePath',
  })
  assert.ok(anchored.issues.some((issue) => issue.code.includes('ANCHOR')))
  assert.ok(anchored.issues.some((issue) => issue.code.includes('ALIAS')))
})

test('derives route card metadata without dropping advanced YAML fields', () => {
  const result = analyzeRouteSource({
    id: crypto.randomUUID(),
    source: `path: /v1/*
type: PROXY
condition: $req.method == 'GET'
proxy_headers:
  X-Test: value
`,
  })
  assert.equal(result.path, '/v1/*')
  assert.equal(result.type, 'PROXY')
  assert.equal(result.condition, "$req.method == 'GET'")
  assert.deepEqual(result.issues, [])
})

test('reports missing and unknown route fields before server validation', () => {
  const result = analyzeRouteSource({
    id: crypto.randomUUID(),
    source: 'status: 200\nfuture_field: true',
  })
  assert.ok(result.issues.some((issue) => issue.code === 'INVALID_ROUTE_PATH'))
  assert.ok(result.issues.some((issue) => issue.code === 'INVALID_ROUTE_TYPE'))
  assert.ok(result.issues.some((issue) => issue.code === 'UNKNOWN_ROUTE_FIELD'))
})
