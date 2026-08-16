import assert from 'node:assert/strict'
import test from 'node:test'

import type { RowDataPacket } from 'mysql2/promise'

import type { DatabasePool } from '../../database/types.js'
import { publicIdToBuffer } from '../../shared/ids.js'
import { ActivationReadRepository } from './read-repository.js'

const releaseId = '50000000-0000-4000-8000-000000000001'
const instanceId = '80000000-0000-4000-8000-000000000001'

test('instance pagination binds LIMIT as an exact decimal value for MySQL 8.4', async () => {
  const calls: Array<{ sql: string; values: unknown[] }> = []
  const responses: RowDataPacket[][] = [
    [{ id: '1', public_id: publicIdToBuffer(releaseId) } as RowDataPacket],
    [
      {
        public_id: publicIdToBuffer(instanceId),
        instance_key: 'access-0',
        activation_status: 'active',
        build_version: 'test',
        build_revision: 'a'.repeat(40),
        evidence_revision: '7',
        route_snapshot_generation: '3',
        route_snapshot_fingerprint: Buffer.from('ab'.repeat(32), 'hex'),
        candidate_status: 'accepted',
        candidate_error_code: null,
        active_md5: Buffer.from('22'.repeat(16), 'hex'),
        active_version: '9',
        observed_at: '2026-08-17 00:00:00.000000',
        expires_at: '2026-08-17 00:01:00.000000',
      } as RowDataPacket,
    ],
    [
      {
        release_id: '1',
        target_count: '1',
        active_count: '1',
        pending_count: '0',
        degraded_count: '0',
        unknown_count: '0',
        evaluated_at: '2026-08-17 00:00:00.000000',
      } as RowDataPacket,
    ],
  ]
  const pool = {
    async execute(sql: string, values: unknown[] = []) {
      calls.push({ sql, values })
      return [responses.shift() ?? [], []]
    },
  } as unknown as DatabasePool
  const repository = new ActivationReadRepository(pool)

  const result = await repository.listReleaseInstances(
    {
      internalId: '1',
      publicId: '10000000-0000-4000-8000-000000000001',
      subject: 'operator',
      displayName: 'Operator',
      platformAdmin: true,
    },
    releaseId,
    null,
    20,
  )

  assert.equal(result?.items[0]?.activeVersion, '9')
  assert.match(calls[1]!.sql, /LIMIT \?/u)
  assert.match(calls[1]!.sql, /release_record\.status = 'published'/u)
  assert.equal(calls[1]!.values.at(-1), '21')
  assert.equal(typeof calls[1]!.values.at(-1), 'string')
})
