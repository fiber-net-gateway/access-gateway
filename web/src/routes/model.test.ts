import assert from 'node:assert/strict'
import test from 'node:test'

import {
  analyzeRouteSource,
  createRouteItem,
  duplicateRouteItem,
  initialRouteModel,
  normalizeExactHost,
} from './model.js'

test('creates an empty ordered YAML route model', () => {
  assert.deepEqual(initialRouteModel(), {
    schemaVersion: 6,
    kind: 'project_routes_yaml',
    hostAliases: [],
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
    routes: [],
  })
})

test('normalizes exact Host aliases and rejects unsafe host syntax', () => {
  assert.equal(normalizeExactHost(' WWW.Example.com. '), 'www.example.com')
  assert.equal(normalizeExactHost('bücher.example'), 'xn--bcher-kva.example')
  assert.equal(normalizeExactHost('*.example.com'), null)
  assert.equal(normalizeExactHost('example.com:443'), null)
})

test('creates independent RESPONSE and PROXY YAML route items', () => {
  const response = createRouteItem('RESPONSE')
  const proxy = createRouteItem('PROXY')
  assert.notEqual(response.id, proxy.id)
  assert.equal(response.format, 'yaml')
  assert.equal(proxy.format, 'yaml')
  assert.match(response.source, /^gzip: true$/mu)
  assert.equal(analyzeRouteSource(response).type, 'RESPONSE')
  assert.equal(analyzeRouteSource(proxy).type, 'PROXY')

  const duplicate = duplicateRouteItem(proxy)
  assert.notEqual(duplicate.id, proxy.id)
  assert.equal(duplicate.source, proxy.source)
})

test('reports malformed documents, duplicate keys, anchors, and non-mapping roots', () => {
  for (const source of ['path: [', 'path: /one\npath: /two\ntype: RESPONSE', '- one\n- two']) {
    const result = analyzeRouteSource({ id: crypto.randomUUID(), format: 'yaml', source })
    assert.ok(result.issues.length > 0, source)
  }

  const anchored = analyzeRouteSource({
    id: crypto.randomUUID(),
    format: 'yaml',
    source: 'path: &routePath /one\ntype: RESPONSE\nrewrite: *routePath',
  })
  assert.ok(anchored.issues.some((issue) => issue.code.includes('ANCHOR')))
  assert.ok(anchored.issues.some((issue) => issue.code.includes('ALIAS')))
})

test('derives route card metadata without dropping advanced YAML fields', () => {
  const result = analyzeRouteSource({
    id: crypto.randomUUID(),
    format: 'yaml',
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
    format: 'yaml',
    source: 'status: 200\nfuture_field: true',
  })
  assert.ok(result.issues.some((issue) => issue.code === 'INVALID_ROUTE_PATH'))
  assert.ok(result.issues.some((issue) => issue.code === 'INVALID_ROUTE_TYPE'))
  assert.ok(result.issues.some((issue) => issue.code === 'UNKNOWN_ROUTE_FIELD'))
})

test('rejects a scalar response_headers block even though it is valid YAML syntax', () => {
  const result = analyzeRouteSource({
    id: crypto.randomUUID(),
    format: 'yaml',
    source: 'path: /\nstatus: 200\ntype: RESPONSE\nresponse_headers:\n  X-Heassf',
  })

  assert.ok(
    result.issues.some(
      (issue) => issue.code === 'INVALID_ROUTE_FIELD_TYPE' && issue.path === 'response_headers',
    ),
  )
})

test('accepts RESPONSE gzip booleans and levels supported by access-server', () => {
  for (const gzip of ['true', 'false', '1', '9']) {
    const body = gzip === 'false' ? '' : '\nbody: { type: TEXT, content: ok }'
    const result = analyzeRouteSource({
      id: crypto.randomUUID(),
      format: 'yaml',
      source: `path: /\ntype: RESPONSE\nstatus: 200${body}\ngzip: ${gzip}`,
    })
    assert.deepEqual(result.issues, [], gzip)
  }
})

test('accepts gzip on dynamic routes and reports unsupported response combinations', () => {
  for (const source of [
    'path: /proxy\ntype: PROXY\nservice: users\ngzip: true',
    'path: /template\ntype: RESPONSE\nstatus: 200\nbody: { type: TEMPLATE, content: value }\ngzip: true',
  ]) {
    const result = analyzeRouteSource({
      id: crypto.randomUUID(),
      format: 'yaml',
      source,
    })
    assert.deepEqual(result.issues, [], source)
  }
  const script = analyzeRouteSource({
    id: crypto.randomUUID(),
    format: 'js',
    path: '/script',
    source: 'resp.send(200, "script")',
    gzip: 1,
  })
  assert.deepEqual(script.issues, [])

  const cases = [
    { source: 'path: /\ntype: RESPONSE\ngzip: 10', code: 'INVALID_ROUTE_GZIP' },
    { source: 'path: /\ntype: RESPONSE\ngzip: null', code: 'INVALID_ROUTE_GZIP' },
    {
      source:
        'path: /\ntype: RESPONSE\nstatus: 206\nbody: { type: TEXT, content: value }\ngzip: true',
      code: 'ROUTE_GZIP_STATUS_CONFLICT',
    },
    {
      source:
        'path: /\ntype: RESPONSE\nstatus: 200\nbody: { type: TEXT, content: value }\ngzip: true\nresponse_headers: { Content-Encoding: br }',
      code: 'ROUTE_GZIP_CONTENT_ENCODING_CONFLICT',
    },
  ]

  for (const testCase of cases) {
    const result = analyzeRouteSource({
      id: crypto.randomUUID(),
      format: 'yaml',
      source: testCase.source,
    })
    assert.ok(
      result.issues.some((issue) => issue.code === testCase.code && issue.path === 'gzip'),
      `${testCase.code}: ${JSON.stringify(result.issues)}`,
    )
  }
})

test('accepts and validates route upstream TLS transport profiles', () => {
  const valid = analyzeRouteSource({
    id: crypto.randomUUID(),
    format: 'yaml',
    source: `path: /secure
type: PROXY
service: orders
upstream_tls:
  generation: 3
  verification: CUSTOM_CA
  ca_pem: test-ca
  server_name: sni.example.com
  verify_name: 192.0.2.1
  client_identity_ref: 123e4567-e89b-42d3-a456-426614174000`,
  })
  assert.deepEqual(valid.issues, [])
  assert.equal(valid.hasUpstreamTls, true)

  for (const testCase of [
    { profile: 'generation: 0', code: 'INVALID_UPSTREAM_TLS_GENERATION' },
    { profile: 'generation: 1\n  unknown: true', code: 'UNKNOWN_UPSTREAM_TLS_FIELD' },
    {
      profile: 'generation: 1\n  verification: CUSTOM_CA',
      code: 'INVALID_UPSTREAM_TLS_CA',
    },
    {
      profile: 'generation: 1\n  verification: LEGACY_INSECURE\n  verify_name: example.com',
      code: 'UPSTREAM_TLS_VERIFY_NAME_CONFLICT',
    },
    {
      profile: 'generation: 1\n  server_name: 127.0.0.1',
      code: 'INVALID_UPSTREAM_TLS_SERVER_NAME',
    },
    {
      profile: "generation: 1\n  verify_name: '::::'",
      code: 'INVALID_UPSTREAM_TLS_VERIFY_NAME',
    },
    {
      profile: 'generation: 1\n  client_identity_ref: not-a-uuid',
      code: 'INVALID_UPSTREAM_TLS_CLIENT_IDENTITY_REF',
    },
  ]) {
    const result = analyzeRouteSource({
      id: crypto.randomUUID(),
      format: 'yaml',
      source: `path: /secure\ntype: PROXY\nservice: orders\nupstream_tls:\n  ${testCase.profile}`,
    })
    assert.ok(
      result.issues.some((issue) => issue.code === testCase.code),
      `${testCase.code}: ${JSON.stringify(result.issues)}`,
    )
  }
})

test('rejects YAML scalar values that cannot be represented safely in JSON', () => {
  const result = analyzeRouteSource({
    id: crypto.randomUUID(),
    format: 'yaml',
    source: 'path: /unsafe\ntype: RESPONSE\nstatus: 9007199254740993',
  })

  assert.ok(result.issues.some((issue) => issue.code === 'ROUTE_VALUE_NOT_JSON_SAFE'))
})

test('creates and analyzes JavaScript routes with external match metadata', () => {
  const route = createRouteItem('JS')
  assert.equal(route.format, 'js')
  if (route.format !== 'js') throw new Error('expected JavaScript route')
  assert.equal(analyzeRouteSource(route).type, 'SCRIPT')
  assert.equal(analyzeRouteSource(route).path, '/script/:id')

  const invalid = analyzeRouteSource({
    ...route,
    method: 'GET POST',
    source: ' ',
  })
  assert.ok(invalid.issues.some((issue) => issue.code === 'INVALID_ROUTE_METHOD'))
  assert.ok(invalid.issues.some((issue) => issue.code === 'EMPTY_ROUTE_SCRIPT'))
})
