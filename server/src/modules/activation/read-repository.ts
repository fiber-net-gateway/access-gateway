import type { RowDataPacket } from 'mysql2/promise'

import type { DatabasePool } from '../../database/types.js'
import { bufferToPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import type { Actor } from '../auth/model.js'
import type {
  ActivationInstanceList,
  ActivationInstanceView,
  ActivationStatus,
  ActivationSummary,
} from './model.js'
import { unknownActivationSummary } from './model.js'

interface SummaryRow extends RowDataPacket {
  release_id: string
  target_count: number | string
  active_count: number | string
  pending_count: number | string
  degraded_count: number | string
  unknown_count: number | string
  evaluated_at: string | null
}

interface ReleaseAccessRow extends RowDataPacket {
  id: string
  public_id: Buffer
}

interface InstanceActivationRow extends RowDataPacket {
  public_id: Buffer
  instance_key: string
  activation_status: ActivationStatus
  build_version: string | null
  build_revision: string | null
  evidence_revision: string | null
  route_snapshot_generation: string | null
  route_snapshot_fingerprint: Buffer | null
  candidate_status: string | null
  candidate_error_code: string | null
  active_md5: Buffer | null
  active_version: number | string | null
  observed_at: string | null
  expires_at: string | null
}

function overallStatus(row: SummaryRow): ActivationStatus {
  const targetCount = Number(row.target_count)
  const activeCount = Number(row.active_count)
  const degradedCount = Number(row.degraded_count)
  const unknownCount = Number(row.unknown_count)
  if (targetCount === 0) return 'unknown'
  if (degradedCount > 0) return 'degraded'
  if (unknownCount > 0) return 'unknown'
  if (activeCount === targetCount) return 'active'
  return 'pending'
}

function toSummary(row: SummaryRow): ActivationSummary {
  return {
    status: overallStatus(row),
    targetCount: Number(row.target_count),
    activeCount: Number(row.active_count),
    pendingCount: Number(row.pending_count),
    degradedCount: Number(row.degraded_count),
    unknownCount: Number(row.unknown_count),
    evaluatedAt: row.evaluated_at ? mysqlDateTimeToRfc3339(row.evaluated_at) : null,
  }
}

export class ActivationReadRepository {
  readonly #pool: DatabasePool

  constructor(pool: DatabasePool) {
    this.#pool = pool
  }

  async summariesForReleases(
    releaseInternalIds: readonly string[],
  ): Promise<ReadonlyMap<string, ActivationSummary>> {
    if (releaseInternalIds.length === 0) return new Map()
    const placeholders = releaseInternalIds.map(() => '?').join(',')
    const [rows] = await this.#pool.execute<SummaryRow[]>(
      `SELECT target.release_id,
              COUNT(*) AS target_count,
              SUM(CASE WHEN activation.expires_at > CURRENT_TIMESTAMP(6)
                        AND activation.status = 'active' THEN 1 ELSE 0 END) AS active_count,
              SUM(CASE WHEN activation.expires_at > CURRENT_TIMESTAMP(6)
                        AND activation.status = 'pending' THEN 1 ELSE 0 END) AS pending_count,
              SUM(CASE WHEN activation.expires_at > CURRENT_TIMESTAMP(6)
                        AND activation.status = 'degraded' THEN 1 ELSE 0 END) AS degraded_count,
              SUM(CASE WHEN activation.expires_at IS NULL
                         OR activation.expires_at <= CURRENT_TIMESTAMP(6)
                        THEN 1 ELSE 0 END) AS unknown_count,
              MAX(CASE WHEN activation.expires_at > CURRENT_TIMESTAMP(6)
                       THEN activation.evaluated_at ELSE NULL END) AS evaluated_at
       FROM release_activation_targets target
       INNER JOIN releases release_record
         ON release_record.id = target.release_id AND release_record.status = 'published'
       INNER JOIN access_server_instances instance_record
         ON instance_record.id = target.instance_id AND instance_record.enabled = TRUE
       LEFT JOIN release_instance_activations activation
         ON activation.release_id = target.release_id
        AND activation.instance_id = target.instance_id
       WHERE target.required_target = TRUE AND target.release_id IN (${placeholders})
       GROUP BY target.release_id`,
      [...releaseInternalIds],
    )
    return new Map(rows.map((row) => [row.release_id, toSummary(row)]))
  }

  async summaryForRelease(releaseInternalId: string): Promise<ActivationSummary> {
    return (
      (await this.summariesForReleases([releaseInternalId])).get(releaseInternalId) ??
      unknownActivationSummary()
    )
  }

  async listReleaseInstances(
    actor: Actor,
    releasePublicId: string,
    cursor: string | null,
    limit: number,
  ): Promise<ActivationInstanceList | null> {
    const permissionJoin = actor.platformAdmin
      ? ''
      : 'INNER JOIN environment_memberships membership ON membership.environment_id = release_record.environment_id AND membership.user_id = ?'
    const accessValues = actor.platformAdmin
      ? [publicIdToBuffer(releasePublicId)]
      : [actor.internalId, publicIdToBuffer(releasePublicId)]
    const [releaseRows] = await this.#pool.execute<ReleaseAccessRow[]>(
      `SELECT release_record.id, release_record.public_id
       FROM releases release_record
       ${permissionJoin}
       WHERE release_record.public_id = ?
       LIMIT 1`,
      accessValues,
    )
    const release = releaseRows[0]
    if (!release) return null

    const values: (string | number | Buffer)[] = [release.id]
    let cursorClause = ''
    if (cursor) {
      cursorClause = 'AND instance_record.public_id > ?'
      values.push(publicIdToBuffer(cursor))
    }
    // mysql2 encodes JavaScript numbers as DOUBLE in binary prepared statements. MySQL 8.4
    // rejects that type for a LIMIT marker, while an exact decimal string remains a bound
    // parameter and is accepted as an integer.
    values.push(String(limit + 1))
    const [rows] = await this.#pool.execute<InstanceActivationRow[]>(
      `SELECT instance_record.public_id, instance_record.instance_key,
              CASE WHEN activation.expires_at > CURRENT_TIMESTAMP(6)
                   THEN activation.status ELSE 'unknown' END AS activation_status,
              observation.build_version, observation.build_revision,
              observation.evidence_revision, observation.route_snapshot_generation,
              observation.route_snapshot_fingerprint,
              CASE release_record.kind
                WHEN 'tls_certificates' THEN observation.tls_candidate_status
                WHEN 'project_decommission' THEN observation.project_list_candidate_status
                ELSE project_observation.candidate_status
              END AS candidate_status,
              CASE release_record.kind
                WHEN 'tls_certificates' THEN observation.tls_error_code
                WHEN 'project_decommission' THEN observation.project_list_error_code
                ELSE project_observation.error_code
              END AS candidate_error_code,
              CASE release_record.kind
                WHEN 'tls_certificates' THEN observation.tls_active_md5
                WHEN 'project_decommission' THEN observation.project_list_md5
                ELSE project_observation.route_md5
              END AS active_md5,
              CASE release_record.kind
                WHEN 'tls_certificates' THEN observation.tls_version
                WHEN 'project_decommission' THEN NULL
                ELSE project_observation.project_version
              END AS active_version,
              observation.observed_at, observation.expires_at
       FROM release_activation_targets target
       INNER JOIN releases release_record ON release_record.id = target.release_id
       INNER JOIN access_server_instances instance_record
         ON instance_record.id = target.instance_id AND instance_record.enabled = TRUE
       LEFT JOIN release_instance_activations activation
         ON activation.release_id = target.release_id
        AND activation.instance_id = target.instance_id
       LEFT JOIN instance_observations observation
         ON observation.id = activation.supporting_observation_id
       LEFT JOIN release_items release_item
         ON release_item.release_id = release_record.id
        AND release_item.kind IN ('project_route', 'project_decommission')
       LEFT JOIN projects project_record ON project_record.id = release_item.project_id
       LEFT JOIN instance_project_observations project_observation
         ON project_observation.instance_observation_id = observation.id
        AND project_observation.project_name = project_record.name
       WHERE target.release_id = ? AND target.required_target = TRUE
         AND release_record.status = 'published'
         ${cursorClause}
       ORDER BY instance_record.public_id
       LIMIT ?`,
      values,
    )
    const hasMore = rows.length > limit
    const page = hasMore ? rows.slice(0, limit) : rows
    const items: ActivationInstanceView[] = page.map((row) => ({
      id: bufferToPublicId(row.public_id),
      instanceKey: row.instance_key,
      status: row.activation_status,
      buildVersion: row.build_version,
      buildRevision: row.build_revision,
      evidenceRevision: row.evidence_revision,
      routeSnapshotGeneration: row.route_snapshot_generation,
      routeSnapshotFingerprintSha256: row.route_snapshot_fingerprint?.toString('hex') ?? null,
      candidateStatus: row.candidate_status,
      candidateErrorCode: row.candidate_error_code,
      activeMd5: row.active_md5?.toString('hex') ?? null,
      activeVersion: row.active_version === null ? null : String(row.active_version),
      observedAt: row.observed_at ? mysqlDateTimeToRfc3339(row.observed_at) : null,
      expiresAt: row.expires_at ? mysqlDateTimeToRfc3339(row.expires_at) : null,
    }))
    return {
      releaseId: bufferToPublicId(release.public_id),
      summary: await this.summaryForRelease(release.id),
      items,
      nextCursor: hasMore ? items.at(-1)!.id : null,
    }
  }
}
