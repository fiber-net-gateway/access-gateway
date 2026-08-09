import assert from 'node:assert/strict'
import test from 'node:test'

import { mysqlDateTimeToRfc3339 } from './time.js'

test('MySQL UTC datetimes are exposed as RFC 3339 without losing microseconds', () => {
  assert.equal(mysqlDateTimeToRfc3339('2026-08-09 12:34:56.123456'), '2026-08-09T12:34:56.123456Z')
  assert.equal(mysqlDateTimeToRfc3339('2026-08-09 12:34:56'), '2026-08-09T12:34:56Z')
  assert.throws(() => mysqlDateTimeToRfc3339('not-a-date'), /invalid/u)
})
