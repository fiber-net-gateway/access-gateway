import assert from 'node:assert/strict'
import test from 'node:test'

import { loadServerConfig } from './env.js'

test('loadServerConfig uses safe local development defaults', () => {
  const config = loadServerConfig({})
  assert.equal(config.host, '127.0.0.1')
  assert.equal(config.port, 3000)
  assert.equal(config.logLevel, 'info')
  assert.equal(config.environment, 'development')
  assert.equal(config.staticRoot, null)
  assert.equal(config.database.enabled, false)
  assert.equal(config.documentEncryption.key, null)
  assert.equal(config.auth.mode, 'development')
  assert.equal(config.nativeValidator.path, null)
  assert.equal(config.publication.enabled, false)
  assert.equal(config.activation.enabled, false)
  assert.deepEqual(config.activation.targets, [])
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
    CONSOLE_STATIC_ROOT: '/opt/access-gateway-console',
    PUBLICATION_WORKER_ENABLED: 'true',
    PUBLICATION_NACOS_ENDPOINT: 'http://rnacos:8848',
  })
  assert.equal(config.host, '0.0.0.0')
  assert.equal(config.port, 3100)
  assert.equal(config.logLevel, 'debug')
  assert.equal(config.database.enabled, true)
  assert.equal(config.database.connectionLimit, 4)
  assert.equal(config.database.sslMode, 'required')
  assert.deepEqual(config.documentEncryption.key, Buffer.alloc(32, 7))
  assert.equal(config.nativeValidator.path, '/opt/access-gateway-validator')
  assert.equal(config.staticRoot, '/opt/access-gateway-console')
  assert.equal(config.publication.enabled, true)
  assert.equal(config.publication.endpointOverride, 'http://rnacos:8848')
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
  assert.throws(
    () => loadServerConfig({ CONSOLE_STATIC_ROOT: './web/dist' }),
    /CONSOLE_STATIC_ROOT/u,
  )
  assert.throws(
    () => loadServerConfig({ PUBLICATION_NACOS_ENDPOINT: 'ftp://rnacos/config' }),
    /PUBLICATION_NACOS_ENDPOINT/u,
  )
  assert.throws(
    () => loadServerConfig({ PUBLICATION_NACOS_ENDPOINT: 'http://user:secret@rnacos' }),
    /PUBLICATION_NACOS_ENDPOINT/u,
  )
})

test('loadServerConfig rejects unsafe production development authentication', () => {
  assert.throws(() => loadServerConfig({ NODE_ENV: 'production' }), /AUTH_MODE=development/u)
})

test('loadServerConfig validates bounded activation collector targets', () => {
  const token = '0123456789abcdef0123456789abcdef'
  const config = loadServerConfig({
    ACTIVATION_COLLECTOR_ENABLED: 'true',
    ACTIVATION_TARGETS_JSON: JSON.stringify([
      {
        environmentCode: 'prod-cn',
        instanceKey: 'access-0',
        endpoint: 'https://access-0.internal:16689/v1/activation-evidence',
        token,
      },
    ]),
  })
  assert.equal(config.activation.enabled, true)
  assert.equal(config.activation.targets[0]?.instanceKey, 'access-0')
  assert.equal(config.activation.targets[0]?.token, token)

  assert.throws(
    () =>
      loadServerConfig({
        ACTIVATION_TARGETS_JSON: JSON.stringify([
          {
            environmentCode: 'prod',
            instanceKey: 'access-0',
            endpoint: 'http://access-0/v1/activation-evidence',
            token,
          },
        ]),
      }),
    /ACTIVATION_TARGETS_JSON/u,
  )
  assert.throws(
    () =>
      loadServerConfig({
        ACTIVATION_COLLECTOR_ENABLED: 'true',
        ACTIVATION_TARGETS_JSON: JSON.stringify([
          {
            environmentCode: 'prod',
            instanceKey: 'access-0',
            endpoint: 'https://user:secret@access-0/v1/activation-evidence',
            token,
          },
        ]),
      }),
    /endpoint/u,
  )

  assert.throws(() => loadServerConfig({ ACTIVATION_CONCURRENCY: '65' }), /ACTIVATION_CONCURRENCY/u)
  assert.throws(
    () =>
      loadServerConfig({
        ACTIVATION_POLL_INTERVAL_MILLIS: '5000',
        ACTIVATION_EVIDENCE_TTL_MILLIS: '5000',
      }),
    /ACTIVATION_EVIDENCE_TTL_MILLIS/u,
  )
  assert.throws(
    () =>
      loadServerConfig({
        ACTIVATION_REQUEST_TIMEOUT_MILLIS: '5000',
        ACTIVATION_LEASE_MILLIS: '5000',
      }),
    /ACTIVATION_LEASE_MILLIS/u,
  )
  assert.throws(
    () =>
      loadServerConfig({
        ACTIVATION_REQUEST_TIMEOUT_MILLIS: '5000',
        ACTIVATION_LEASE_MILLIS: '9000',
      }),
    /ACTIVATION_LEASE_MILLIS/u,
  )
  assert.throws(
    () =>
      loadServerConfig({
        ACTIVATION_MAX_PAGES: '1',
        ACTIVATION_MAX_PROJECTS: '257',
      }),
    /ACTIVATION_MAX_PAGES/u,
  )
})

test('loadServerConfig rejects an explicitly empty or malformed host', () => {
  assert.throws(() => loadServerConfig({ APP_HOST: ' ' }), /APP_HOST/u)
  assert.throws(() => loadServerConfig({ APP_HOST: 'bad host' }), /APP_HOST/u)
  assert.throws(() => loadServerConfig({ APP_HOST: 'http://localhost' }), /APP_HOST/u)
})
