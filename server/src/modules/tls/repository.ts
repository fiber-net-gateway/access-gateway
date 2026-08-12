import type { RowDataPacket } from 'mysql2/promise'

import type { DatabasePool } from '../../database/types.js'
import { bufferToPublicId } from '../../shared/ids.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import type { TlsSniCertificateSummary, TlsSniResolutionView } from './model.js'

interface TlsSniMatchRow extends RowDataPacket {
  certificate_series_id: string
  match_kind: 'exact' | 'wildcard'
  certificate_public_id: Buffer
  certificate_name: string
  version_no: number
  lifecycle_state: 'active' | 'superseded'
  fingerprint_sha256: Buffer
  not_after: string
}

function certificateStatus(
  lifecycleState: 'active' | 'superseded',
  notAfterValue: string,
  now = Date.now(),
): TlsSniCertificateSummary['status'] {
  if (lifecycleState === 'superseded') return 'superseded'
  const notAfter = Date.parse(mysqlDateTimeToRfc3339(notAfterValue))
  if (notAfter <= now) return 'expired'
  return notAfter - now <= 30 * 24 * 60 * 60 * 1_000 ? 'expiring' : 'valid'
}

function toCertificateSummary(row: TlsSniMatchRow): TlsSniCertificateSummary {
  return {
    id: bufferToPublicId(row.certificate_public_id),
    name: row.certificate_name,
    version: row.version_no,
    status: certificateStatus(row.lifecycle_state, row.not_after),
    notAfter: mysqlDateTimeToRfc3339(row.not_after),
    fingerprintSha256: row.fingerprint_sha256.toString('hex'),
    runtimeDeploymentStatus: 'activation_unknown',
  }
}

export class TlsSniRepository {
  readonly #pool: DatabasePool

  constructor(pool: DatabasePool) {
    this.#pool = pool
  }

  async resolve(environmentInternalId: string, serverName: string): Promise<TlsSniResolutionView> {
    const [rows] = await this.#pool.execute<TlsSniMatchRow[]>(
      `SELECT
         selector.certificate_series_id,
         selector.match_kind,
         series_record.public_id AS certificate_public_id,
         series_record.display_name AS certificate_name,
         current_version.version_no,
         current_version.lifecycle_state,
         current_version.fingerprint_sha256,
         current_version.not_after
       FROM certificate_san_selectors selector
       INNER JOIN certificate_series series_record
         ON series_record.id = selector.certificate_series_id
       INNER JOIN certificates current_version
         ON current_version.id = series_record.current_version_id
       WHERE selector.environment_id = ?
         AND series_record.archived_at IS NULL
         AND (
           (selector.match_kind = 'exact' AND selector.match_value = ?)
           OR
           (
             selector.match_kind = 'wildcard'
             AND ? LIKE CONCAT('%.', selector.match_value)
             AND LENGTH(?) - LENGTH(REPLACE(?, '.', '')) = selector.wildcard_dot_count
           )
         )
       ORDER BY CASE selector.match_kind WHEN 'exact' THEN 0 ELSE 1 END,
                series_record.id`,
      [environmentInternalId, serverName, serverName, serverName, serverName],
    )
    const preferredKind = rows.some((row) => row.match_kind === 'exact') ? 'exact' : 'wildcard'
    const seen = new Set<string>()
    const matches = rows
      .filter((row) => row.match_kind === preferredKind)
      .filter((row) => {
        if (seen.has(row.certificate_series_id)) return false
        seen.add(row.certificate_series_id)
        return true
      })
      .map(toCertificateSummary)
    return {
      serverName,
      resolutionStatus:
        matches.length === 0 ? 'uncovered' : matches.length === 1 ? 'matched' : 'conflict',
      matchKind: matches.length > 0 ? preferredKind : null,
      certificate: matches.length === 1 ? matches[0]! : null,
      matches,
      runtimeDeploymentStatus: 'activation_unknown',
    }
  }
}
