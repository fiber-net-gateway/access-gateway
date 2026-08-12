import assert from 'node:assert/strict'
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'

import { buildApp } from './app.js'

test('GET /api/health returns deterministic service metadata', async (context) => {
  const app = buildApp()
  context.after(() => app.close())

  const response = await app.inject({
    method: 'GET',
    url: '/api/health',
  })

  assert.equal(response.statusCode, 200)
  assert.deepEqual(response.json(), {
    status: 'ok',
    service: 'access-gateway-console-api',
    version: '0.1.0',
  })
})

test('unknown API routes return a stable machine-readable error', async (context) => {
  const app = buildApp()
  context.after(() => app.close())

  const response = await app.inject({
    method: 'GET',
    url: '/api/missing',
  })

  assert.equal(response.statusCode, 404)
  const body = response.json()
  assert.equal(body.error.code, 'NOT_FOUND')
  assert.equal(body.error.message, 'Route not found')
  assert.equal(typeof body.error.requestId, 'string')
})

test('serves the Console assets and SPA fallback without hiding API 404s', async (context) => {
  const staticRoot = await mkdtemp(join(tmpdir(), 'access-gateway-console-'))
  await mkdir(join(staticRoot, 'assets'))
  await writeFile(join(staticRoot, 'index.html'), '<main>Console shell</main>')
  await writeFile(join(staticRoot, 'assets', 'app.js'), 'console.log("loaded")')

  const app = buildApp({ staticRoot })
  context.after(async () => {
    await app.close()
    await rm(staticRoot, { recursive: true, force: true })
  })

  const root = await app.inject({ method: 'GET', url: '/' })
  assert.equal(root.statusCode, 200)
  assert.match(root.body, /Console shell/u)

  const asset = await app.inject({ method: 'GET', url: '/assets/app.js' })
  assert.equal(asset.statusCode, 200)
  assert.equal(asset.body, 'console.log("loaded")')

  const spaRoute = await app.inject({ method: 'GET', url: '/projects/demo' })
  assert.equal(spaRoute.statusCode, 200)
  assert.match(spaRoute.body, /Console shell/u)

  const missingApi = await app.inject({ method: 'GET', url: '/api/missing' })
  assert.equal(missingApi.statusCode, 404)
  assert.equal(missingApi.json().error.code, 'NOT_FOUND')
})

test('readiness and persistent APIs fail closed when MySQL is unconfigured', async (context) => {
  const app = buildApp()
  context.after(() => app.close())

  const readiness = await app.inject({ method: 'GET', url: '/api/health/ready' })
  assert.equal(readiness.statusCode, 503)
  assert.equal(readiness.json().status, 'degraded')
  assert.equal(readiness.json().dependencies.database.status, 'unconfigured')

  const projects = await app.inject({ method: 'GET', url: '/api/projects' })
  assert.equal(projects.statusCode, 503)
  assert.equal(projects.json().error.code, 'DATABASE_UNCONFIGURED')

  const certificates = await app.inject({ method: 'GET', url: '/api/certificates' })
  assert.equal(certificates.statusCode, 503)
  assert.equal(certificates.json().error.code, 'DATABASE_UNCONFIGURED')

  const sniResolution = await app.inject({
    method: 'GET',
    url: '/api/tls/sni-resolution?serverName=api.example.com',
  })
  assert.equal(sniResolution.statusCode, 503)
  assert.equal(sniResolution.json().error.code, 'DATABASE_UNCONFIGURED')
})

test('YAML route APIs accept schema v4 and reject the legacy whole-project request shape', async (context) => {
  const app = buildApp()
  context.after(() => app.close())
  const projectId = '00000000-0000-4000-8000-000000000001'
  const routeId = '00000000-0000-4000-8000-000000000002'

  const accepted = await app.inject({
    method: 'POST',
    url: `/api/projects/${projectId}/routes/validate`,
    payload: {
      model: {
        schemaVersion: 4,
        kind: 'project_routes_yaml',
        networkPolicy: {
          source: 'route',
          httpsRedirect: 'off',
          allowedCidrs: [],
          deniedCidrs: [],
        },
        routes: [{ id: routeId, source: 'path: /health\ntype: RESPONSE\nstatus: 200' }],
      },
    },
  })
  assert.equal(accepted.statusCode, 503)
  assert.equal(accepted.json().error.code, 'DATABASE_UNCONFIGURED')

  const rejected = await app.inject({
    method: 'POST',
    url: `/api/projects/${projectId}/routes/validate`,
    payload: {
      model: { schemaVersion: 1, kind: 'project_route', hosts: [], routes: [] },
    },
  })
  assert.equal(rejected.statusCode, 400)
  assert.equal(rejected.json().error.code, 'VALIDATION_ERROR')
})

test('configuration version and Release APIs are registered and fail closed without storage', async (context) => {
  const app = buildApp()
  context.after(() => app.close())
  const projectId = '00000000-0000-4000-8000-000000000001'
  const versionId = '00000000-0000-4000-8000-000000000002'
  const routeId = '00000000-0000-4000-8000-000000000003'

  const versions = await app.inject({
    method: 'GET',
    url: `/api/projects/${projectId}/configuration-versions`,
  })
  assert.equal(versions.statusCode, 503)
  assert.equal(versions.json().error.code, 'DATABASE_UNCONFIGURED')

  const saved = await app.inject({
    method: 'POST',
    url: `/api/projects/${projectId}/configuration-versions`,
    headers: { 'if-match': '"0"', 'idempotency-key': 'test-save-v1' },
    payload: {
      baseVersionId: null,
      changeSummary: 'Create V1',
      model: {
        schemaVersion: 4,
        kind: 'project_routes_yaml',
        networkPolicy: {
          source: 'route',
          httpsRedirect: 'off',
          allowedCidrs: [],
          deniedCidrs: [],
        },
        routes: [{ id: routeId, source: 'path: /\ntype: RESPONSE\nstatus: 200' }],
      },
    },
  })
  assert.equal(saved.statusCode, 503)
  assert.equal(saved.json().error.code, 'DATABASE_UNCONFIGURED')

  const restoredAfterEditing = await app.inject({
    method: 'POST',
    url: `/api/projects/${projectId}/configuration-versions/${versionId}/restorations`,
    headers: { 'if-match': '"1"', 'idempotency-key': 'test-restore-edited-v2' },
    payload: {
      baseVersionId: versionId,
      changeSummary: 'Use V1 as an editing source',
      model: {
        schemaVersion: 4,
        kind: 'project_routes_yaml',
        networkPolicy: {
          source: 'route',
          httpsRedirect: 'off',
          allowedCidrs: [],
          deniedCidrs: [],
        },
        routes: [{ id: routeId, source: 'path: /edited\ntype: RESPONSE\nstatus: 200' }],
      },
    },
  })
  assert.equal(restoredAfterEditing.statusCode, 503)
  assert.equal(restoredAfterEditing.json().error.code, 'DATABASE_UNCONFIGURED')

  const invalidEditedRestoration = await app.inject({
    method: 'POST',
    url: `/api/projects/${projectId}/configuration-versions/${versionId}/restorations`,
    headers: { 'if-match': '"1"', 'idempotency-key': 'test-restore-invalid' },
    payload: {
      baseVersionId: versionId,
      changeSummary: 'Invalid derived content',
      model: { schemaVersion: 1, kind: 'project_route', routes: [] },
    },
  })
  assert.equal(invalidEditedRestoration.statusCode, 400)
  assert.equal(invalidEditedRestoration.json().error.code, 'VALIDATION_ERROR')

  const release = await app.inject({
    method: 'POST',
    url: `/api/projects/${projectId}/releases`,
    headers: { 'idempotency-key': 'test-release-v1' },
    payload: {
      sourceVersionId: versionId,
      expectedCurrentVersionId: versionId,
      title: 'Publish V1',
    },
  })
  assert.equal(release.statusCode, 503)
  assert.equal(release.json().error.code, 'DATABASE_UNCONFIGURED')

  const tlsRelease = await app.inject({
    method: 'POST',
    url: '/api/tls/releases',
    headers: { 'idempotency-key': 'test-tls-release-v1' },
    payload: { defaultCertificateId: '00000000-0000-4000-8000-000000000004' },
  })
  assert.equal(tlsRelease.statusCode, 503)
  assert.equal(tlsRelease.json().error.code, 'DATABASE_UNCONFIGURED')
})
