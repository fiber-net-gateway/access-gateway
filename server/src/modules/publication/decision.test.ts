import assert from 'node:assert/strict'
import test from 'node:test'

import { aggregateReleasePublication, decidePublication } from './decision.js'

const missing = { exists: false, sha256: null }
const base = { exists: true, sha256: 'base' }

test('publication decision is idempotent and detects external conflicts', () => {
  assert.equal(
    decidePublication({
      operation: 'upsert',
      before: { exists: true, sha256: 'target' },
      base,
      targetSha256: 'target',
      previouslyWritten: false,
    }),
    'verify_target',
  )
  assert.equal(
    decidePublication({
      operation: 'upsert',
      before: base,
      base,
      targetSha256: 'target',
      previouslyWritten: false,
    }),
    'write',
  )
  assert.equal(
    decidePublication({
      operation: 'upsert',
      before: { exists: true, sha256: 'external' },
      base,
      targetSha256: 'target',
      previouslyWritten: false,
    }),
    'conflict',
  )
  assert.equal(
    decidePublication({
      operation: 'remove',
      before: missing,
      base,
      targetSha256: null,
      previouslyWritten: true,
    }),
    'verify_target',
  )
})

test('publication aggregation never hides a partial target change as a failure', () => {
  assert.equal(aggregateReleasePublication(['verified', 'verified'], true), 'published')
  assert.equal(aggregateReleasePublication(['verified', 'failed'], true), 'partially_published')
  assert.equal(aggregateReleasePublication(['pending', 'failed'], false), 'publish_failed')
  assert.equal(aggregateReleasePublication(['pending'], false), 'publishing')
})
