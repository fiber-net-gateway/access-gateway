import assert from 'node:assert/strict'
import test from 'node:test'

import { canTransitionRelease, requireReleaseTransition } from './state.js'

test('release state machine permits only declared forward transitions', () => {
  assert.equal(canTransitionRelease('creating', 'validating'), true)
  assert.equal(canTransitionRelease('ready', 'queued'), true)
  assert.equal(canTransitionRelease('publishing', 'partially_published'), true)
  assert.equal(canTransitionRelease('published', 'superseded'), true)
  assert.equal(canTransitionRelease('published', 'publishing'), false)
  assert.equal(canTransitionRelease('canceled', 'queued'), false)
})

test('invalid release transitions return a stable conflict', () => {
  assert.throws(
    () => requireReleaseTransition('validation_failed', 'ready'),
    (error: unknown) =>
      typeof error === 'object' &&
      error !== null &&
      (error as { code?: string }).code === 'INVALID_RELEASE_TRANSITION',
  )
})
