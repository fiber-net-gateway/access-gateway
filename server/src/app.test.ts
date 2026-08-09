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
  assert.deepEqual(response.json(), {
    error: {
      code: 'NOT_FOUND',
      message: 'Route not found',
    },
  })
})
