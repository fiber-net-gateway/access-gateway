import assert from 'node:assert/strict'
import test from 'node:test'

import { fallbackAccessConfigLimits } from '../../integrations/native-validator/limits.js'
import type { ProjectRoutesModel } from './model.js'
import { compileProjectRoutes } from './compiler.js'

function model(...sources: string[]): ProjectRoutesModel {
  return {
    schemaVersion: 5,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
    routes: sources.map((source, index) => ({
      id: `00000000-0000-4000-8000-${String(index + 1).padStart(12, '0')}`,
      format: 'yaml',
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
        id: routeId,
        format: 'yaml',
        source: 'path: &shared /one\ntype: RESPONSE\nrewrite: *shared',
      },
    ],
  })
  assert.equal(result.compiled, null)
  assert.ok(result.issues.every((issue) => issue.routeId === routeId))
  assert.ok(result.issues.some((issue) => issue.code === 'YAML_ANCHOR_NOT_ALLOWED'))
  assert.ok(result.issues.some((issue) => issue.code === 'YAML_ALIAS_NOT_ALLOWED'))
})

test('compiles YAML method and mixed JavaScript routes in source order', () => {
  const result = compileProjectRoutes('api.example.com', {
    ...model('path: /items\nmethod: GET\ntype: RESPONSE\nstatus: 200'),
    routes: [
      ...model('path: /items\nmethod: GET\ntype: RESPONSE\nstatus: 200').routes,
      {
        id: '00000000-0000-4000-8000-000000000002',
        format: 'js',
        path: '/items/:id',
        method: 'POST',
        source: 'return {id: $path.id};',
      },
    ],
  })

  assert.deepEqual(result.issues, [])
  const payload = JSON.parse(result.compiled!.payloadText) as { routes: unknown[] }
  assert.deepEqual(payload.routes, [
    { method: 'GET', path: '/items', status: 200, type: 'RESPONSE' },
    {
      method: 'POST',
      path: '/items/:id',
      script: 'return {id: $path.id};',
      type: 'SCRIPT',
    },
  ])
})

test('rejects invalid methods and incomplete JavaScript route metadata', () => {
  const invalidYaml = compileProjectRoutes(
    'api.example.com',
    model('path: /items\nmethod: GET POST\ntype: RESPONSE\nstatus: 200'),
  )
  assert.ok(invalidYaml.issues.some((issue) => issue.code === 'INVALID_ROUTE_METHOD'))

  const invalidScript = compileProjectRoutes('api.example.com', {
    ...model(),
    routes: [
      {
        id: '00000000-0000-4000-8000-000000000001',
        format: 'js',
        path: '/script',
        source: '   ',
      },
    ],
  })
  assert.ok(invalidScript.issues.some((issue) => issue.code === 'EMPTY_ROUTE_SCRIPT'))
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

test('compiles boolean and numeric RESPONSE gzip settings without coercion', () => {
  const result = compileProjectRoutes(
    'api.example.com',
    model(
      'path: /default\ntype: RESPONSE\nstatus: 200\nbody: { type: TEXT, content: ok }\ngzip: true',
      'path: /fast\ntype: RESPONSE\nstatus: 200\nbody: { type: BASE64, content: YQ== }\ngzip: 1',
      'path: /identity\ntype: RESPONSE\nstatus: 200\ngzip: false',
    ),
  )

  assert.deepEqual(result.issues, [])
  const payload = JSON.parse(result.compiled!.payloadText) as { routes: unknown[] }
  assert.deepEqual(payload.routes, [
    {
      body: { content: 'ok', type: 'TEXT' },
      gzip: true,
      path: '/default',
      status: 200,
      type: 'RESPONSE',
    },
    {
      body: { content: 'YQ==', type: 'BASE64' },
      gzip: 1,
      path: '/fast',
      status: 200,
      type: 'RESPONSE',
    },
    { gzip: false, path: '/identity', status: 200, type: 'RESPONSE' },
  ])
})

test('rejects gzip values and combinations unsupported by access-server', () => {
  const cases = [
    { source: 'path: /\ntype: RESPONSE\ngzip: 0', code: 'INVALID_ROUTE_GZIP' },
    { source: 'path: /\ntype: RESPONSE\ngzip: "1"', code: 'INVALID_ROUTE_GZIP' },
    {
      source: 'path: /\ntype: PROXY\nservice: users\ngzip: false',
      code: 'ROUTE_GZIP_TYPE_CONFLICT',
    },
    {
      source:
        'path: /\ntype: RESPONSE\nstatus: 200\nbody: { type: TEMPLATE, content: "${$req.method}" }\ngzip: true',
      code: 'ROUTE_GZIP_BODY_CONFLICT',
    },
    {
      source:
        'path: /\ntype: RESPONSE\nstatus: 204\nbody: { type: TEXT, content: body }\ngzip: true',
      code: 'ROUTE_GZIP_STATUS_CONFLICT',
    },
    {
      source:
        'path: /\ntype: RESPONSE\nstatus: 200\nbody: { type: TEXT, content: body }\ngzip: true\nresponse_headers: { content-encoding: br }',
      code: 'ROUTE_GZIP_CONTENT_ENCODING_CONFLICT',
    },
    {
      source: 'path: /\ntype: RESPONSE\nstatus: 200\ngzip: true',
      code: 'ROUTE_GZIP_BODY_CONFLICT',
    },
  ]

  for (const testCase of cases) {
    const result = compileProjectRoutes('api.example.com', model(testCase.source))
    assert.equal(result.compiled, null)
    assert.ok(
      result.issues.some((issue) => issue.code === testCase.code && issue.path === 'gzip'),
      `${testCase.code}: ${JSON.stringify(result.issues)}`,
    )
  }
})

test('compiles a strict upstream TLS transport profile without exposing a file path', () => {
  const result = compileProjectRoutes(
    'api.example.com',
    model(`path: /secure
type: PROXY
service: orders
upstream_tls:
  generation: 7
  verification: CUSTOM_CA
  ca_pem: test-ca
  server_name: sni.example.com
  verify_name: identity.example.com`),
  )

  assert.deepEqual(result.issues, [])
  const payload = JSON.parse(result.compiled!.payloadText) as {
    routes: Array<Record<string, unknown>>
  }
  assert.deepEqual(payload.routes[0]?.upstream_tls, {
    ca_pem: 'test-ca',
    generation: 7,
    server_name: 'sni.example.com',
    verification: 'CUSTOM_CA',
    verify_name: 'identity.example.com',
  })
})

test('rejects malformed, misleading, and oversized upstream TLS profiles', () => {
  const cases = [
    { source: 'generation: 0', code: 'INVALID_UPSTREAM_TLS_GENERATION' },
    { source: 'generation: 1\n  future: true', code: 'UNKNOWN_UPSTREAM_TLS_FIELD' },
    {
      source: 'generation: 1\n  verification: CUSTOM_CA',
      code: 'INVALID_UPSTREAM_TLS_CA',
    },
    {
      source: 'generation: 1\n  verification: SYSTEM_CA\n  ca_pem: unexpected',
      code: 'UPSTREAM_TLS_CA_CONFLICT',
    },
    {
      source: 'generation: 1\n  verification: LEGACY_INSECURE\n  verify_name: example.com',
      code: 'UPSTREAM_TLS_VERIFY_NAME_CONFLICT',
    },
    { source: 'generation: 1\n  server_name: 127.0.0.1', code: 'INVALID_UPSTREAM_TLS_SERVER_NAME' },
  ]
  for (const testCase of cases) {
    const result = compileProjectRoutes(
      'api.example.com',
      model(`path: /secure\ntype: PROXY\nservice: orders\nupstream_tls:\n  ${testCase.source}`),
    )
    assert.ok(
      result.issues.some((issue) => issue.code === testCase.code),
      `${testCase.code}: ${JSON.stringify(result.issues)}`,
    )
  }

  const response = compileProjectRoutes(
    'api.example.com',
    model('path: /\ntype: RESPONSE\nstatus: 200\nupstream_tls: { generation: 1 }'),
  )
  assert.ok(response.issues.some((issue) => issue.code === 'UPSTREAM_TLS_ROUTE_TYPE_CONFLICT'))

  const limits = {
    ...fallbackAccessConfigLimits,
    projectRoute: {
      ...fallbackAccessConfigLimits.projectRoute,
      maxUpstreamTlsCaPemBytes: 3,
    },
  }
  const oversized = compileProjectRoutes(
    'api.example.com',
    model(
      'path: /\ntype: PROXY\nservice: orders\nupstream_tls: { generation: 1, verification: CUSTOM_CA, ca_pem: four }',
    ),
    1,
    limits,
  )
  assert.ok(
    oversized.issues.some(
      (issue) => issue.path === 'upstream_tls.ca_pem' && issue.code === 'limit_exceeded',
    ),
  )

  const profileLimited = compileProjectRoutes(
    'api.example.com',
    model(
      'path: /one\ntype: PROXY\nservice: orders\nupstream_tls: { generation: 1 }',
      'path: /two\ntype: PROXY\nservice: orders\nupstream_tls: { generation: 2 }',
    ),
    1,
    {
      ...fallbackAccessConfigLimits,
      projectRoute: {
        ...fallbackAccessConfigLimits.projectRoute,
        maxUpstreamTlsProfiles: 1,
      },
    },
  )
  assert.ok(
    profileLimited.issues.some(
      (issue) => issue.path === 'upstream_tls' && issue.code === 'limit_exceeded',
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
      httpsRedirect: 'off',
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
      httpsRedirect: 'off',
      allowedCidrs: ['10.0.0.0/99'],
      deniedCidrs: [],
    },
  })

  assert.equal(result.compiled, null)
  assert.ok(result.issues.some((issue) => issue.code === 'INVALID_NETWORK_POLICY_CIDR'))
  assert.ok(result.issues.some((issue) => issue.code === 'ROUTE_NETWORK_POLICY_CONFLICT'))
})

test('compiles every HTTPS redirect setting to the native HostStrategy wire value', () => {
  const expected = {
    off: 'S_NOT_MUST',
    '301': 'S_301',
    '302': 'S_302',
    '307': 'S_307',
    '308': 'S_308',
  } as const

  for (const [httpsRedirect, strategy] of Object.entries(expected)) {
    const candidate = model('path: /\ntype: RESPONSE\nstatus: 200')
    const result = compileProjectRoutes('api.example.com', {
      ...candidate,
      networkPolicy: {
        ...candidate.networkPolicy,
        httpsRedirect: httpsRedirect as keyof typeof expected,
      },
    })
    assert.deepEqual(result.issues, [])
    const payload = JSON.parse(result.compiled!.payloadText) as {
      host: Record<string, { https: string }>
    }
    assert.equal(payload.host['api.example.com']?.https, strategy)
  }
})

test('enforces native UTF-8 and compiled payload limits before validation', () => {
  const scriptLimits = {
    ...fallbackAccessConfigLimits,
    projectRoute: {
      ...fallbackAccessConfigLimits.projectRoute,
      maxScriptBytes: 3,
    },
  }
  const oversizedScript = compileProjectRoutes(
    'api.example.com',
    {
      ...model(),
      routes: [
        {
          id: '00000000-0000-4000-8000-000000000001',
          format: 'js',
          path: '/',
          source: '返回',
        },
      ],
    },
    1,
    scriptLimits,
  )
  assert.equal(oversizedScript.compiled, null)
  assert.ok(
    oversizedScript.issues.some(
      (issue) => issue.code === 'limit_exceeded' && issue.path === 'source',
    ),
  )

  const payloadLimits = {
    ...fallbackAccessConfigLimits,
    projectRoute: {
      ...fallbackAccessConfigLimits.projectRoute,
      maxPayloadBytes: 100,
    },
  }
  const oversizedPayload = compileProjectRoutes(
    'api.example.com',
    model('path: /\ntype: RESPONSE\nstatus: 200'),
    1,
    payloadLimits,
  )
  assert.equal(oversizedPayload.compiled, null)
  assert.ok(
    oversizedPayload.issues.some(
      (issue) => issue.code === 'limit_exceeded' && issue.path === 'payload',
    ),
  )
})

test('enforces native structured entry limits on compiled YAML routes', () => {
  const limits = {
    ...fallbackAccessConfigLimits,
    projectRoute: {
      ...fallbackAccessConfigLimits.projectRoute,
      maxHeaderEntries: 1,
    },
  }
  const result = compileProjectRoutes(
    'api.example.com',
    model('path: /\ntype: RESPONSE\nstatus: 200\nresponse_headers: { X-One: one, X-Two: two }'),
    1,
    limits,
  )

  assert.equal(result.compiled, null)
  assert.ok(
    result.issues.some(
      (issue) => issue.code === 'limit_exceeded' && issue.path === 'response_headers',
    ),
  )

  const clusterLimits = {
    ...fallbackAccessConfigLimits,
    projectRoute: {
      ...fallbackAccessConfigLimits.projectRoute,
      maxClusterBytes: 3,
    },
  }
  const serviceCluster = compileProjectRoutes(
    'api.example.com',
    model('path: /\ntype: PROXY\nservice: users/gray'),
    1,
    clusterLimits,
  )
  assert.equal(serviceCluster.compiled, null)
  assert.ok(
    serviceCluster.issues.some(
      (issue) => issue.code === 'limit_exceeded' && issue.path === 'service',
    ),
  )
})
