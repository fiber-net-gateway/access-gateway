import assert from 'node:assert/strict'
import test from 'node:test'

import type { DatabasePool } from '../../database/types.js'
import { publicIdToBuffer } from '../../shared/ids.js'
import { TlsSniRepository } from './repository.js'

const wildcardCertificateId = '00000000-0000-4000-8000-000000000001'
const exactCertificateId = '00000000-0000-4000-8000-000000000002'
const otherExactCertificateId = '00000000-0000-4000-8000-000000000003'

function selectorRow(input: {
  seriesId: string
  certificateId: string
  matchKind: 'exact' | 'wildcard'
}) {
  return {
    certificate_series_id: input.seriesId,
    match_kind: input.matchKind,
    certificate_public_id: publicIdToBuffer(input.certificateId),
    certificate_name: `${input.matchKind} certificate`,
    version_no: 1,
    lifecycle_state: 'active',
    fingerprint_sha256: Buffer.alloc(32, 1),
    not_after: '2099-08-12 00:00:00.000000',
  }
}

function repositoryWithRows(rows: readonly ReturnType<typeof selectorRow>[]): TlsSniRepository {
  const pool = {
    execute: async () => [rows, []],
  } as unknown as DatabasePool
  return new TlsSniRepository(pool)
}

test('SNI resolution prefers exact selectors over wildcard candidates', async () => {
  const repository = repositoryWithRows([
    selectorRow({
      seriesId: '1',
      certificateId: wildcardCertificateId,
      matchKind: 'wildcard',
    }),
    selectorRow({
      seriesId: '2',
      certificateId: exactCertificateId,
      matchKind: 'exact',
    }),
  ])

  const resolution = await repository.resolve('10', 'api.example.com')

  assert.equal(resolution.resolutionStatus, 'matched')
  assert.equal(resolution.matchKind, 'exact')
  assert.equal(resolution.certificate?.id, exactCertificateId)
  assert.deepEqual(
    resolution.matches.map((certificate) => certificate.id),
    [exactCertificateId],
  )
})

test('SNI resolution fails closed if selector uniqueness is ever violated', async () => {
  const repository = repositoryWithRows([
    selectorRow({
      seriesId: '2',
      certificateId: exactCertificateId,
      matchKind: 'exact',
    }),
    selectorRow({
      seriesId: '3',
      certificateId: otherExactCertificateId,
      matchKind: 'exact',
    }),
  ])

  const resolution = await repository.resolve('10', 'api.example.com')

  assert.equal(resolution.resolutionStatus, 'conflict')
  assert.equal(resolution.matchKind, 'exact')
  assert.equal(resolution.certificate, null)
  assert.deepEqual(
    resolution.matches.map((certificate) => certificate.id),
    [exactCertificateId, otherExactCertificateId],
  )
})
