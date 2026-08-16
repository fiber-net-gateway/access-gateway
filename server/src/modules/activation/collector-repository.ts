import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import type { ActivationTargetConfig } from '../../config/env.js'
import type { DatabasePool, SqlExecutor } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import type { ActivationEvidence } from '../../integrations/activation-evidence/model.js'
import { createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { decideInstanceActivation, type ActivationReleaseResource } from './decision.js'

interface InstanceRow extends RowDataPacket {
  id: string
}

interface ReleaseResourceRow extends RowDataPacket {
  release_id: string
  kind: ActivationReleaseResource['kind']
  data_id: string
  group_name: string
  operation: ActivationReleaseResource['operation']
  verified_nacos_md5: Buffer | null
  allocated_project_version: number | null
  project_name: string | null
}

export interface ActivationPollClaim {
  instanceInternalId: string
  leaseToken: Buffer
  target: ActivationTargetConfig
}

export interface ActivationCollectorRepositoryOptions {
  pollIntervalMillis: number
  evidenceTtlMillis: number
  leaseMillis: number
  owner: string
}

function digest(value: string | null): Buffer | null {
  return value ? Buffer.from(value, 'hex') : null
}

function observedDate(unixMillis: number): Date | null {
  return unixMillis > 0 ? new Date(unixMillis) : null
}

function groupReleaseResources(
  rows: readonly ReleaseResourceRow[],
): ReadonlyMap<string, readonly ActivationReleaseResource[]> {
  const releases = new Map<string, ActivationReleaseResource[]>()
  for (const row of rows) {
    const resources = releases.get(row.release_id) ?? []
    resources.push({
      kind: row.kind,
      dataId: row.data_id,
      group: row.group_name,
      operation: row.operation,
      verifiedNacosMd5: row.verified_nacos_md5?.toString('hex') ?? null,
      allocatedProjectVersion: row.allocated_project_version,
      projectName: row.project_name,
    })
    releases.set(row.release_id, resources)
  }
  return releases
}

export class ActivationCollectorRepository {
  readonly #pool: DatabasePool
  readonly #options: ActivationCollectorRepositoryOptions

  constructor(pool: DatabasePool, options: ActivationCollectorRepositoryOptions) {
    this.#pool = pool
    this.#options = options
  }

  async synchronizeTargets(targets: readonly ActivationTargetConfig[]): Promise<void> {
    await withTransaction(this.#pool, async (transaction) => {
      await transaction.execute(
        `UPDATE access_server_instances
         SET enabled = FALSE, updated_at = CURRENT_TIMESTAMP(6), lock_version = lock_version + 1
         WHERE source = 'static_config' AND enabled = TRUE`,
      )

      for (const target of targets) {
        const [environmentRows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
          "SELECT id FROM environments WHERE code = ? AND status = 'active' LIMIT 1",
          [target.environmentCode],
        )
        const environment = environmentRows[0]
        if (!environment) {
          throw new Error(`Activation target environment is unavailable: ${target.environmentCode}`)
        }
        await transaction.execute(
          `INSERT INTO access_server_instances
            (public_id, environment_id, instance_key, source, status_endpoint,
             status_secret_ref_id, enabled, poll_interval_millis, created_by, next_poll_at)
           VALUES (?, ?, ?, 'static_config', ?, NULL, TRUE, ?, NULL, CURRENT_TIMESTAMP(6))
           ON DUPLICATE KEY UPDATE
             source = 'static_config', status_endpoint = VALUES(status_endpoint),
             status_secret_ref_id = NULL, enabled = TRUE,
             poll_interval_millis = VALUES(poll_interval_millis),
             next_poll_at = LEAST(next_poll_at, CURRENT_TIMESTAMP(6)),
             updated_at = CURRENT_TIMESTAMP(6), lock_version = lock_version + 1`,
          [
            publicIdToBuffer(createPublicId()),
            environment.id,
            target.instanceKey,
            target.endpoint,
            this.#options.pollIntervalMillis,
          ],
        )
      }

      await transaction.execute(
        `UPDATE release_activation_targets target
         INNER JOIN access_server_instances instance_record ON instance_record.id = target.instance_id
         SET target.required_target = instance_record.enabled
         WHERE instance_record.source = 'static_config'`,
      )
      await transaction.execute(
        `INSERT INTO release_activation_targets (release_id, instance_id, required_target)
         SELECT release_record.id, instance_record.id, TRUE
         FROM releases release_record
         INNER JOIN access_server_instances instance_record
           ON instance_record.environment_id = release_record.environment_id
          AND instance_record.enabled = TRUE
         WHERE release_record.status = 'published'
         ON DUPLICATE KEY UPDATE required_target = TRUE`,
      )
    })
  }

  async claim(target: ActivationTargetConfig): Promise<ActivationPollClaim | null> {
    let claim: ActivationPollClaim | null = null
    await withTransaction(this.#pool, async (transaction) => {
      const [rows] = await transaction.execute<InstanceRow[]>(
        `SELECT instance_record.id
         FROM access_server_instances instance_record
         INNER JOIN environments environment_record
           ON environment_record.id = instance_record.environment_id
         WHERE environment_record.code = ? AND instance_record.instance_key = ?
           AND instance_record.enabled = TRUE
           AND instance_record.next_poll_at <= CURRENT_TIMESTAMP(6)
           AND (instance_record.lease_expires_at IS NULL
                OR instance_record.lease_expires_at <= CURRENT_TIMESTAMP(6))
         LIMIT 1
         FOR UPDATE SKIP LOCKED`,
        [target.environmentCode, target.instanceKey],
      )
      const instance = rows[0]
      if (!instance) return
      const leaseToken = publicIdToBuffer(createPublicId())
      await transaction.execute(
        `UPDATE access_server_instances
         SET lease_owner = ?, lease_token = ?,
             lease_expires_at = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND)
         WHERE id = ?`,
        [this.#options.owner, leaseToken, this.#options.leaseMillis * 1_000, instance.id],
      )
      claim = { instanceInternalId: instance.id, leaseToken, target }
    })
    return claim
  }

  async persistSuccess(claim: ActivationPollClaim, evidence: ActivationEvidence): Promise<void> {
    await withTransaction(this.#pool, async (transaction) => {
      await this.requireLease(transaction, claim)
      const evidenceTtlMicros = String(this.#options.evidenceTtlMillis * 1_000)
      const projectList = evidence.accessConfig.projectList
      const gray = evidence.gray.resource
      const tls = evidence.tls.resource
      const [result] = await transaction.execute<ResultSetHeader>(
        `INSERT INTO instance_observations
          (public_id, instance_id, contract_version, evidence_revision, poll_result,
           build_version, build_revision, runtime_state, instance_started_at,
           route_watcher_state, route_readiness_state, route_snapshot_generation,
           route_snapshot_fingerprint, route_snapshot_published_at,
           project_list_md5, project_list_observed_md5, project_list_candidate_status,
           project_list_error_code, project_list_observed_at, project_list_active_at,
           project_list_ready, gray_md5, gray_observed_md5, gray_candidate_status,
           gray_error_code, gray_generation, gray_rule_count,
           tls_enabled, tls_observed_md5, tls_active_md5, tls_candidate_status,
           tls_error_code, tls_version, tls_certificate_count,
           discovery_client_state, discovery_config_state, discovery_naming_state,
           discovery_ready_services, discovery_selectable_endpoints,
           discovery_logical_clusters, discovery_selector_leases,
           error_code, observed_at, expires_at)
         VALUES (?, ?, 1, ?, 'success', ?, ?, 'running', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                 ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL,
                 CURRENT_TIMESTAMP(6), DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND))`,
        [
          publicIdToBuffer(createPublicId()),
          claim.instanceInternalId,
          evidence.evidenceRevision,
          evidence.instance.buildVersion,
          evidence.instance.buildRevision,
          observedDate(evidence.instance.startedAtUnixMillis),
          evidence.accessConfig.watcherState,
          evidence.accessConfig.readinessState,
          evidence.routeSnapshot.generation,
          Buffer.from(evidence.routeSnapshot.fingerprintSha256, 'hex'),
          observedDate(evidence.routeSnapshot.publishedAtUnixMillis),
          digest(projectList.activeMd5),
          digest(projectList.observedMd5),
          projectList.candidateStatus,
          projectList.failure?.code ?? null,
          observedDate(projectList.observedAtUnixMillis),
          observedDate(projectList.activeAtUnixMillis),
          evidence.accessConfig.readinessState === 'ready',
          digest(gray.activeMd5),
          digest(gray.observedMd5),
          gray.candidateStatus,
          gray.failure?.code ?? null,
          evidence.gray.generation,
          evidence.gray.ruleCount,
          evidence.tls.enabled,
          digest(tls.observedMd5),
          digest(tls.activeMd5),
          tls.candidateStatus,
          tls.failure?.code ?? null,
          evidence.tls.version,
          evidence.tls.certificateCount,
          evidence.discovery.clientState,
          evidence.discovery.configServiceState,
          evidence.discovery.namingServiceState,
          evidence.discovery.readyServices,
          evidence.discovery.selectableEndpoints,
          evidence.discovery.logicalClusters,
          evidence.discovery.selectorLeases,
          evidenceTtlMicros,
        ],
      )
      const observationId = result.insertId.toString()
      await this.insertProjects(transaction, observationId, evidence)
      await this.finishPoll(transaction, claim, true)
      await this.ensureCurrentTargets(transaction, claim.instanceInternalId)
      await this.evaluateCurrentReleases(
        transaction,
        claim.instanceInternalId,
        observationId,
        evidenceTtlMicros,
        evidence,
        null,
      )
      await this.pruneHistory(transaction, claim.instanceInternalId)
    })
  }

  async persistFailure(claim: ActivationPollClaim, errorCode: string): Promise<void> {
    await withTransaction(this.#pool, async (transaction) => {
      await this.requireLease(transaction, claim)
      const evidenceTtlMicros = String(this.#options.evidenceTtlMillis * 1_000)
      const [result] = await transaction.execute<ResultSetHeader>(
        `INSERT INTO instance_observations
          (public_id, instance_id, poll_result, error_code, observed_at, expires_at)
         VALUES (?, ?, 'failure', ?, CURRENT_TIMESTAMP(6),
                 DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND))`,
        [
          publicIdToBuffer(createPublicId()),
          claim.instanceInternalId,
          errorCode,
          evidenceTtlMicros,
        ],
      )
      await this.finishPoll(transaction, claim, false)
      await this.ensureCurrentTargets(transaction, claim.instanceInternalId)
      await this.evaluateCurrentReleases(
        transaction,
        claim.instanceInternalId,
        result.insertId.toString(),
        evidenceTtlMicros,
        null,
        errorCode,
      )
      await this.pruneHistory(transaction, claim.instanceInternalId)
    })
  }

  private async requireLease(transaction: SqlExecutor, claim: ActivationPollClaim): Promise<void> {
    const [rows] = await transaction.execute<InstanceRow[]>(
      `SELECT id FROM access_server_instances
       WHERE id = ? AND lease_token = ? AND lease_expires_at > CURRENT_TIMESTAMP(6)
       FOR UPDATE`,
      [claim.instanceInternalId, claim.leaseToken],
    )
    if (!rows[0]) throw new Error('Activation collector lease was lost')
  }

  private async insertProjects(
    transaction: SqlExecutor,
    observationId: string,
    evidence: ActivationEvidence,
  ): Promise<void> {
    if (evidence.projects.length === 0) return
    const batchSize = 256
    for (let offset = 0; offset < evidence.projects.length; offset += batchSize) {
      const projects = evidence.projects.slice(offset, offset + batchSize)
      const placeholders = projects
        .map(() => '(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)')
        .join(',')
      const values = projects.flatMap((project) => [
        observationId,
        project.name,
        project.subscriptionState,
        project.observedVersion,
        digest(project.observedMd5),
        project.activeVersion,
        digest(project.activeMd5),
        project.activeSnapshotGeneration,
        project.activeLoaded,
        observedDate(project.observedAtUnixMillis),
        observedDate(project.activeAtUnixMillis),
        project.candidateStatus,
        project.failure?.code ?? null,
        project.failure?.field ?? null,
        project.failure?.offset ?? null,
      ])
      await transaction.execute(
        `INSERT INTO instance_project_observations
          (instance_observation_id, project_name, subscription_state,
           observed_project_version, observed_route_md5, project_version, route_md5,
           active_snapshot_generation, active_loaded, observed_at, active_at,
           candidate_status, error_code, error_field, error_offset)
         VALUES ${placeholders}`,
        values,
      )
    }
  }

  private async finishPoll(
    transaction: SqlExecutor,
    claim: ActivationPollClaim,
    successful: boolean,
  ): Promise<void> {
    const [result] = await transaction.execute<ResultSetHeader>(
      `UPDATE access_server_instances
       SET last_polled_at = CURRENT_TIMESTAMP(6),
           last_seen_at = IF(?, CURRENT_TIMESTAMP(6), last_seen_at),
           next_poll_at = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND),
           lease_owner = NULL, lease_token = NULL, lease_expires_at = NULL,
           updated_at = CURRENT_TIMESTAMP(6)
       WHERE id = ? AND lease_token = ?`,
      [
        successful,
        this.#options.pollIntervalMillis * 1_000,
        claim.instanceInternalId,
        claim.leaseToken,
      ],
    )
    if (result.affectedRows !== 1) throw new Error('Activation collector lease was lost')
  }

  private async ensureCurrentTargets(
    transaction: SqlExecutor,
    instanceInternalId: string,
  ): Promise<void> {
    await transaction.execute(
      `INSERT INTO release_activation_targets (release_id, instance_id, required_target)
       SELECT release_record.id, instance_record.id, TRUE
       FROM access_server_instances instance_record
       INNER JOIN releases release_record
         ON release_record.environment_id = instance_record.environment_id
       WHERE instance_record.id = ? AND instance_record.enabled = TRUE
         AND release_record.status = 'published'
       ON DUPLICATE KEY UPDATE required_target = TRUE`,
      [instanceInternalId],
    )
  }

  private async evaluateCurrentReleases(
    transaction: SqlExecutor,
    instanceInternalId: string,
    observationId: string,
    evidenceTtlMicros: string,
    evidence: ActivationEvidence | null,
    pollErrorCode: string | null,
  ): Promise<void> {
    const [rows] = await transaction.execute<ReleaseResourceRow[]>(
      `SELECT target.release_id, resource.kind, resource.data_id, resource.group_name,
              resource.operation, resource.verified_nacos_md5,
              resource.allocated_project_version, project_record.name AS project_name
       FROM release_activation_targets target
       INNER JOIN releases release_record ON release_record.id = target.release_id
       INNER JOIN release_resources resource
         ON resource.release_id = release_record.id AND resource.required_resource = TRUE
       LEFT JOIN projects project_record ON project_record.id = resource.project_id
       WHERE target.instance_id = ? AND target.required_target = TRUE
         AND release_record.status = 'published'
       ORDER BY target.release_id, resource.publish_order, resource.id`,
      [instanceInternalId],
    )
    for (const [releaseId, resources] of groupReleaseResources(rows)) {
      const status = decideInstanceActivation({ pollErrorCode, evidence, resources })
      await transaction.execute(
        `INSERT INTO release_instance_activations
          (release_id, instance_id, status, supporting_observation_id, evaluated_at, expires_at)
         VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP(6),
                 DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL ? MICROSECOND))
         ON DUPLICATE KEY UPDATE
           status = VALUES(status), supporting_observation_id = VALUES(supporting_observation_id),
           evaluated_at = VALUES(evaluated_at), expires_at = VALUES(expires_at)`,
        [releaseId, instanceInternalId, status, observationId, evidenceTtlMicros],
      )
    }
  }

  private async pruneHistory(transaction: SqlExecutor, instanceInternalId: string): Promise<void> {
    const [rows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
      `SELECT id FROM instance_observations
       WHERE instance_id = ? AND observed_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 7 DAY)
         AND id NOT IN (
           SELECT supporting_observation_id FROM release_instance_activations
           WHERE instance_id = ? AND supporting_observation_id IS NOT NULL
         )
       ORDER BY id LIMIT 32`,
      [instanceInternalId, instanceInternalId],
    )
    if (rows.length === 0) return
    const ids = rows.map((row) => row.id)
    const placeholders = ids.map(() => '?').join(',')
    await transaction.execute(
      `DELETE FROM instance_project_observations
       WHERE instance_observation_id IN (${placeholders})`,
      ids,
    )
    await transaction.execute(
      `DELETE FROM instance_observations WHERE id IN (${placeholders})`,
      ids,
    )
  }
}
