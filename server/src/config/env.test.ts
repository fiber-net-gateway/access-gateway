import assert from 'node:assert/strict'
import test from 'node:test'

import { loadServerConfig } from './env.js'

test('loadServerConfig uses safe local development defaults', () => {
  const config = loadServerConfig({})
  assert.equal(config.host, '127.0.0.1')
  assert.equal(config.port, 3000)
  assert.equal(config.logLevel, 'info')
  assert.equal(config.environment, 'development')
  assert.equal(config.database.enabled, false)
  assert.equal(config.documentEncryption.key, null)
  assert.equal(config.auth.mode, 'development')
  assert.equal(config.nativeValidator.path, null)
})

test('loadServerConfig parses explicit database and validator values', () => {
  const key = Buffer.alloc(32, 7).toString('base64')
  const config = loadServerConfig({
    APP_HOST: '0.0.0.0',
    APP_PORT: '3100',
    LOG_LEVEL: 'DEBUG',
    MYSQL_ENABLED: 'true',
    MYSQL_PASSWORD: 'test-password',
    MYSQL_CONNECTION_LIMIT: '4',
    MYSQL_SSL_MODE: 'required',
    DOCUMENT_ENCRYPTION_KEY_BASE64: key,
    NATIVE_VALIDATOR_PATH: '/opt/access-gateway-validator',
  })
  assert.equal(config.host, '0.0.0.0')
  assert.equal(config.port, 3100)
  assert.equal(config.logLevel, 'debug')
  assert.equal(config.database.enabled, true)
  assert.equal(config.database.connectionLimit, 4)
  assert.equal(config.database.sslMode, 'required')
  assert.deepEqual(config.documentEncryption.key, Buffer.alloc(32, 7))
  assert.equal(config.nativeValidator.path, '/opt/access-gateway-validator')
})

test('loadServerConfig rejects invalid ports, log levels, and database secrets', () => {
  assert.throws(() => loadServerConfig({ APP_PORT: '0' }), /APP_PORT/u)
  assert.throws(() => loadServerConfig({ APP_PORT: '12.5' }), /APP_PORT/u)
  assert.throws(() => loadServerConfig({ APP_PORT: '' }), /APP_PORT/u)
  assert.throws(() => loadServerConfig({ LOG_LEVEL: 'verbose' }), /LOG_LEVEL/u)
  assert.throws(() => loadServerConfig({ MYSQL_ENABLED: 'true' }), /MYSQL_PASSWORD/u)
  assert.throws(
    () =>
      loadServerConfig({
        MYSQL_ENABLED: 'true',
        MYSQL_PASSWORD: 'present',
        DOCUMENT_ENCRYPTION_KEY_BASE64: 'too-short',
      }),
    /DOCUMENT_ENCRYPTION_KEY_BASE64/u,
  )
  assert.throws(
    () =>
      loadServerConfig({
        MYSQL_ENABLED: 'true',
        MYSQL_PASSWORD: 'present',
        DOCUMENT_ENCRYPTION_KEY_BASE64: `${Buffer.alloc(32).toString('base64')}junk`,
      }),
    /DOCUMENT_ENCRYPTION_KEY_BASE64/u,
  )
  assert.throws(
    () => loadServerConfig({ NATIVE_VALIDATOR_PATH: './validator' }),
    /NATIVE_VALIDATOR_PATH/u,
  )
})

test('loadServerConfig rejects unsafe production development authentication', () => {
  assert.throws(() => loadServerConfig({ NODE_ENV: 'production' }), /AUTH_MODE=development/u)
})

test('loadServerConfig rejects an explicitly empty or malformed host', () => {
  assert.throws(() => loadServerConfig({ APP_HOST: ' ' }), /APP_HOST/u)
  assert.throws(() => loadServerConfig({ APP_HOST: 'bad host' }), /APP_HOST/u)
  assert.throws(() => loadServerConfig({ APP_HOST: 'http://localhost' }), /APP_HOST/u)
})
