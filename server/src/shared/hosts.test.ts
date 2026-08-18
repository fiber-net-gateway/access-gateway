import assert from 'node:assert/strict'
import test from 'node:test'

import { isCanonicalExactHost, normalizeExactHost } from './hosts.js'

test('normalizes exact DNS hosts for project aliases', () => {
  assert.equal(normalizeExactHost('  WWW.Example.com. '), 'www.example.com')
  assert.equal(normalizeExactHost('bücher.example'), 'xn--bcher-kva.example')
  assert.equal(isCanonicalExactHost('www.example.com'), true)
  assert.equal(isCanonicalExactHost('WWW.example.com'), false)
})

test('rejects URL, wildcard, IP, and malformed host inputs', () => {
  for (const value of [
    '*.example.com',
    'https://example.com',
    'example.com:443',
    'example.com/path',
    '127.0.0.1',
    '[::1]',
    'a..example.com',
    '-bad.example.com',
  ]) {
    assert.equal(normalizeExactHost(value), null, value)
  }
})
