import assert from 'node:assert/strict'
import test from 'node:test'

import { hostPatternConflicts } from './service.js'

test('detects exact and wildcard host conflicts for release preflight', () => {
  assert.equal(hostPatternConflicts('API.Example.com.', 'api.example.com'), true)
  assert.equal(hostPatternConflicts('*.example.com', 'API.Example.com.'), true)
  assert.equal(hostPatternConflicts('*.example.com', 'example.com'), false)
  assert.equal(hostPatternConflicts('*', 'any.example.com'), true)
  assert.equal(hostPatternConflicts('other.example.com', 'api.example.com'), false)
})
