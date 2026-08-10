import assert from 'node:assert/strict'
import test from 'node:test'

import { AppError } from '../../shared/errors.js'
import { normalizeProjectDomain } from './service.js'

test('normalizes project domains to their lower-case ASCII form', () => {
  assert.equal(normalizeProjectDomain('  API.Example.COM.  '), 'api.example.com')
  assert.equal(normalizeProjectDomain('例子.测试'), 'xn--fsqu00a.xn--0zwm56d')
  assert.equal(normalizeProjectDomain('localhost'), 'localhost')
})

test('rejects values that are not exact DNS hostnames', () => {
  for (const value of [
    'https://example.com',
    '*.example.com',
    'example.com:443',
    '127.0.0.1',
    '-bad.example',
    'bad-.example',
    'a..example',
  ]) {
    assert.throws(
      () => normalizeProjectDomain(value),
      (error: unknown) => error instanceof AppError && error.code === 'INVALID_PROJECT_DOMAIN',
      value,
    )
  }
})
