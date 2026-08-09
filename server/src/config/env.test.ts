import assert from 'node:assert/strict'
import test from 'node:test'

import { loadServerConfig } from './env.js'

test('loadServerConfig uses local development defaults', () => {
  assert.deepEqual(loadServerConfig({}), {
    host: '127.0.0.1',
    port: 3000,
    logLevel: 'info',
  })
})

test('loadServerConfig parses explicit values', () => {
  assert.deepEqual(
    loadServerConfig({
      APP_HOST: '0.0.0.0',
      APP_PORT: '3100',
      LOG_LEVEL: 'DEBUG',
    }),
    {
      host: '0.0.0.0',
      port: 3100,
      logLevel: 'debug',
    },
  )
})

test('loadServerConfig rejects invalid ports and log levels', () => {
  assert.throws(() => loadServerConfig({ APP_PORT: '0' }), /APP_PORT/u)
  assert.throws(() => loadServerConfig({ APP_PORT: '12.5' }), /APP_PORT/u)
  assert.throws(() => loadServerConfig({ APP_PORT: '' }), /APP_PORT/u)
  assert.throws(() => loadServerConfig({ LOG_LEVEL: 'verbose' }), /LOG_LEVEL/u)
  assert.throws(() => loadServerConfig({ LOG_LEVEL: ' ' }), /LOG_LEVEL/u)
})

test('loadServerConfig rejects an explicitly empty or malformed host', () => {
  assert.throws(() => loadServerConfig({ APP_HOST: ' ' }), /APP_HOST/u)
  assert.throws(() => loadServerConfig({ APP_HOST: 'bad host' }), /APP_HOST/u)
  assert.throws(() => loadServerConfig({ APP_HOST: 'http://localhost' }), /APP_HOST/u)
})
