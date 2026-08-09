import assert from 'node:assert/strict'
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
