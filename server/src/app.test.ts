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

  const environments = await app.inject({ method: 'GET', url: '/api/environments' })
  assert.equal(environments.statusCode, 503)
  assert.equal(environments.json().error.code, 'DATABASE_UNCONFIGURED')
})
