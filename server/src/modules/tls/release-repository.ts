import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import type { NacosResourceValue } from '../../integrations/nacos/model.js'
import { conflict, notFound, unprocessable } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type {
  QueueTlsCertificatePublicationResult,
  TlsCertificateReleaseView,
} from './release-model.js'

export const TLS_CERTIFICATES_DATA_ID = 'ploto.unified-access.tls-certificates'
export const TLS_CERTIFICATES_GROUP = 'ACCESS-SERVER'

interface CertificateMaterialRow extends RowDataPacket {
  series_public_id: Buffer
  version_public_id: Buffer
  certificate_document_id: string
  private_key_document_id: string
  currently_valid: number | string
}

interface ReleaseRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  sequence_no: string
  status: TlsCertificateReleaseView['status']
  default_certificate_public_id: string
  certificate_count: number
  wire_sha256: Buffer
  resource_public_id: Buffer
  resource_status: TlsCertificateReleaseView['resource']['status']
  verified_sha256: Buffer | null
  verified_at: string | null
  job_public_id: Buffer | null
  job_state: string | null
  created_at: string
  published_at: string | null
}

interface ExistingReleaseRow extends RowDataPacket {
  public_id: Buffer
  request_sha256: Buffer
}

const selectRelease = `
  SELECT
    rel.id AS internal_id, rel.public_id, rel.sequence_no, rel.status,
    JSON_UNQUOTE(JSON_EXTRACT(item.diff_summary_json, '$.defaultCertificateId'))
      AS default_certificate_public_id,
    CAST(JSON_UNQUOTE(JSON_EXTRACT(item.diff_summary_json, '$.certificateCount')) AS UNSIGNED)
      AS certificate_count,
    resource.target_sha256 AS wire_sha256,
    resource.public_id AS resource_public_id, resource.status AS resource_status,
    resource.verified_sha256, resource.verified_at,
    job.public_id AS job_public_id, job.state AS job_state,
    rel.created_at, rel.published_at
  FROM releases rel
  INNER JOIN release_items item
    ON item.release_id = rel.id AND item.kind = 'tls_certificates'
  INNER JOIN release_resources resource
    ON resource.release_id = rel.id AND resource.kind = 'tls_certificates'
  LEFT JOIN publication_jobs job ON job.release_id = rel.id
`

function publishedTlsSnapshotVersion(base: NacosResourceValue): bigint {
  if (!base.exists || !base.content) return 0n
  try {
    const value: unknown = JSON.parse(base.content)
    if (!value || typeof value !== 'object' || Array.isArray(value)) return 0n
    const version = (value as { version?: unknown }).version
    if (
      typeof version !== 'number' ||
      !Number.isSafeInteger(version) ||
      version < 1 ||
      version > 2_147_483_647
    ) {
      return 0n
    }
    return BigInt(version)
  } catch {
    return 0n
  }
}

export class TlsCertificateReleaseRepository {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, documents: DocumentRepository, audit = new AuditRepository()) {
    this.#pool = pool
    this.#documents = documents
    this.#audit = audit
  }

  async create(
    actor: Actor,
    environmentInternalId: string,
    defaultCertificateId: string,
    idempotencyKey: string,
    requestSha256: Buffer,
    base: NacosResourceValue,
    requestId: string,
  ): Promise<TlsCertificateReleaseView> {
    let releasePublicId = createPublicId()
    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [existingRows] = await transaction.execute<ExistingReleaseRow[]>(
          `SELECT public_id, request_sha256
           FROM releases
           WHERE environment_id = ? AND created_by = ? AND idempotency_key = ?
           LIMIT 1`,
          [environmentInternalId, actor.internalId, idempotencyKey],
        )
        const existing = existingRows[0]
        if (existing) {
          if (!existing.request_sha256.equals(requestSha256)) {
            throw conflict(
              'IDEMPOTENCY_KEY_REUSED',
              'The idempotency key was already used with a different TLS release request',
            )
          }
          releasePublicId = bufferToPublicId(existing.public_id)
          return
        }

        const [environmentRows] = await transaction.execute<
          (RowDataPacket & { last_release_sequence: string })[]
        >('SELECT last_release_sequence FROM environments WHERE id = ? FOR UPDATE', [
          environmentInternalId,
        ])
        const environment = environmentRows[0]
        if (!environment) throw notFound('Deployment workspace')
        const lastReleaseSequence = BigInt(environment.last_release_sequence)
        const baseVersion = publishedTlsSnapshotVersion(base)
        const sequence =
          (lastReleaseSequence > baseVersion ? lastReleaseSequence : baseVersion) + 1n
        if (sequence > 2_147_483_647n) {
          throw conflict(
            'TLS_SNAPSHOT_VERSION_EXHAUSTED',
            'The TLS snapshot version range is exhausted',
          )
        }

        const [certificates] = await transaction.execute<CertificateMaterialRow[]>(
          `SELECT
             series_record.public_id AS series_public_id,
             current_version.public_id AS version_public_id,
             current_version.certificate_document_id,
             current_version.private_key_document_id,
             (current_version.not_before <= CURRENT_TIMESTAMP(6)
              AND current_version.not_after > CURRENT_TIMESTAMP(6)) AS currently_valid
           FROM certificate_series series_record
           INNER JOIN certificates current_version
             ON current_version.id = series_record.current_version_id
           WHERE series_record.environment_id = ? AND series_record.archived_at IS NULL
           ORDER BY series_record.id
           FOR SHARE`,
          [environmentInternalId],
        )
        if (certificates.length === 0 || certificates.length > 128) {
          throw unprocessable(
            'TLS_CERTIFICATE_COUNT_INVALID',
            'A TLS release requires between 1 and 128 current certificates',
          )
        }
        if (certificates.some((certificate) => Number(certificate.currently_valid) !== 1)) {
          throw unprocessable(
            'TLS_CERTIFICATE_NOT_CURRENTLY_VALID',
            'Every certificate in the TLS snapshot must be currently valid',
          )
        }
        const defaultCertificate = certificates.find(
          (certificate) => bufferToPublicId(certificate.series_public_id) === defaultCertificateId,
        )
        if (!defaultCertificate) throw notFound('Default certificate')

        const material: {
          id: string
          certificatePem: string
          privateKeyPem: string
        }[] = []
        for (const certificate of certificates) {
          // A transaction owns one MySQL connection. Keep these reads sequential.
          const certificatePem = await this.#documents.decryptByInternalId(
            transaction,
            certificate.certificate_document_id,
          )
          const privateKeyPem = await this.#documents.decryptByInternalId(
            transaction,
            certificate.private_key_document_id,
          )
          if (!certificatePem || !privateKeyPem) {
            throw new Error('Certificate material could not be decrypted')
          }
          if (certificatePem.byteLength > 128 * 1024 || privateKeyPem.byteLength > 32 * 1024) {
            throw unprocessable(
              'TLS_CERTIFICATE_MATERIAL_TOO_LARGE',
              'Certificate material exceeds the access-server snapshot limits',
            )
          }
          material.push({
            id: bufferToPublicId(certificate.version_public_id),
            certificatePem: certificatePem.toString('utf8'),
            privateKeyPem: privateKeyPem.toString('utf8'),
          })
        }
        const payload = canonicalJson({
          schemaVersion: 1,
          version: Number(sequence),
          defaultCertificate: bufferToPublicId(defaultCertificate.version_public_id),
          certificates: material,
        })
        if (Buffer.byteLength(payload, 'utf8') > 4 * 1024 * 1024) {
          throw unprocessable(
            'TLS_SNAPSHOT_TOO_LARGE',
            'The compiled TLS certificate snapshot exceeds 4 MiB',
          )
        }
        const encryptedPayload = this.#documents.encrypt(Buffer.from(payload, 'utf8'))
        const payloadDocument = await this.#documents.insert(transaction, {
          environmentInternalId,
          purpose: 'release_payload',
          contentType: 'application/json',
          schemaVersion: 1,
          encrypted: encryptedPayload,
        })
        let baseDocumentId: string | null = null
        if (base.exists && base.content !== null) {
          const baseDocument = await this.#documents.insert(transaction, {
            environmentInternalId,
            purpose: 'nacos_observation',
            contentType: 'application/json',
            schemaVersion: 1,
            encrypted: this.#documents.encrypt(Buffer.from(base.content, 'utf8')),
          })
          baseDocumentId = baseDocument.internalId
        }
        const [observation] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO nacos_resource_observations
            (public_id, environment_id, data_id, group_name, resource_exists,
             payload_document_id, nacos_md5, sha256, source, client_result, fetched_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'release_preflight', 'success', CURRENT_TIMESTAMP(6))`,
          [
            publicIdToBuffer(createPublicId()),
            environmentInternalId,
            TLS_CERTIFICATES_DATA_ID,
            TLS_CERTIFICATES_GROUP,
            base.exists,
            baseDocumentId,
            base.md5 ? Buffer.from(base.md5, 'hex') : null,
            base.sha256 ? Buffer.from(base.sha256, 'hex') : null,
          ],
        )

        await transaction.execute(
          'UPDATE environments SET last_release_sequence = ? WHERE id = ?',
          [sequence.toString(), environmentInternalId],
        )
        const [release] = await transaction.execute<ResultSetHeader>(
          `INSERT INTO releases
            (public_id, environment_id, sequence_no, kind, status, title, description,
             compiler_revision, idempotency_key, request_sha256, validation_errors_json,
             native_validator_contract, native_validator_revision, created_by, ready_at)
           VALUES (?, ?, ?, 'tls_certificates', 'ready', ?, '', 'tls-snapshot-v1', ?, ?,
                   JSON_ARRAY(), 1, 'tls-snapshot-v1', ?, CURRENT_TIMESTAMP(6))`,
          [
            publicIdToBuffer(releasePublicId),
            environmentInternalId,
            sequence.toString(),
            `TLS certificates #${sequence.toString()}`,
            idempotencyKey,
            requestSha256,
            actor.internalId,
          ],
        )
        const releaseInternalId = release.insertId.toString()
        await transaction.execute(
          `INSERT INTO release_items
            (public_id, release_id, project_id, kind, draft_revision_id, source_relation,
             model_document_id, allocated_project_version, change_kind, diff_summary_json)
           VALUES (?, ?, NULL, 'tls_certificates', NULL, NULL, ?, ?, 'update', ?)`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            payloadDocument.internalId,
            Number(sequence),
            JSON.stringify({ defaultCertificateId, certificateCount: certificates.length }),
          ],
        )
        await transaction.execute(
          `INSERT INTO release_resources
            (public_id, release_id, project_id, kind, data_id, group_name, operation,
             publish_order, required_resource, payload_document_id, base_observation_id,
             target_sha256, allocated_project_version, status)
           VALUES (?, ?, NULL, 'tls_certificates', ?, ?, 'upsert', 10, TRUE, ?, ?, ?, ?, 'pending')`,
          [
            publicIdToBuffer(createPublicId()),
            releaseInternalId,
            TLS_CERTIFICATES_DATA_ID,
            TLS_CERTIFICATES_GROUP,
            payloadDocument.internalId,
            observation.insertId.toString(),
            sha256(payload),
            Number(sequence),
          ],
        )
        await this.#audit.append(transaction, {
          environmentInternalId,
          actorInternalId: actor.internalId,
          eventType: 'tls.release.created',
          targetType: 'release',
          targetPublicId: releasePublicId,
          requestId,
          result: 'success',
          summary: {
            sequence: sequence.toString(),
            defaultCertificateId,
            certificateCount: certificates.length,
            wireSha256: sha256(payload).toString('hex'),
          },
        })
      },
      { retryOnDeadlock: true },
    )
    const result = await this.find(releasePublicId)
    if (!result) throw new Error('TLS release could not be reloaded')
    return result
  }

  async list(environmentInternalId: string): Promise<readonly TlsCertificateReleaseView[]> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease}
       WHERE rel.environment_id = ? AND rel.kind = 'tls_certificates'
       ORDER BY rel.id DESC LIMIT 100`,
      [environmentInternalId],
    )
    return rows.map((row) => this.toView(row))
  }

  async find(
    publicId: string,
    environmentInternalId?: string,
  ): Promise<TlsCertificateReleaseView | null> {
    const [rows] = await this.#pool.execute<ReleaseRow[]>(
      `${selectRelease}
       WHERE rel.public_id = ? AND rel.kind = 'tls_certificates'
         ${environmentInternalId ? 'AND rel.environment_id = ?' : ''}
       LIMIT 1`,
      environmentInternalId
        ? [publicIdToBuffer(publicId), environmentInternalId]
        : [publicIdToBuffer(publicId)],
    )
    return rows[0] ? this.toView(rows[0]) : null
  }

  async queuePublication(
    actor: Actor,
    releasePublicId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueTlsCertificatePublicationResult> {
    let jobPublicId = createPublicId()
    let jobState = 'queued'
    await withTransaction(this.#pool, async (transaction) => {
      const [releaseRows] = await transaction.execute<
        (RowDataPacket & { id: string; environment_id: string; status: string })[]
      >(
        `SELECT id, environment_id, status
         FROM releases WHERE public_id = ? AND kind = 'tls_certificates' FOR UPDATE`,
        [publicIdToBuffer(releasePublicId)],
      )
      const release = releaseRows[0]
      if (!release) throw notFound('TLS release')
      const [existingRows] = await transaction.execute<
        (RowDataPacket & { public_id: Buffer; state: string })[]
      >('SELECT public_id, state FROM publication_jobs WHERE release_id = ?', [release.id])
      const existing = existingRows[0]
      if (existing) {
        jobPublicId = bufferToPublicId(existing.public_id)
        jobState = existing.state
        return
      }
      if (release.status !== 'ready') {
        throw conflict('RELEASE_NOT_READY', 'Only a ready TLS release can be published')
      }
      await transaction.execute(
        `INSERT INTO publication_jobs
          (public_id, release_id, state, requested_by, requested_at, next_run_at)
         VALUES (?, ?, 'queued', ?, CURRENT_TIMESTAMP(6), CURRENT_TIMESTAMP(6))`,
        [publicIdToBuffer(jobPublicId), release.id, actor.internalId],
      )
      await transaction.execute(
        `UPDATE releases SET status = 'queued', lock_version = lock_version + 1 WHERE id = ?`,
        [release.id],
      )
      await this.#audit.append(transaction, {
        environmentInternalId: release.environment_id,
        actorInternalId: actor.internalId,
        eventType: 'publication.queued',
        targetType: 'release',
        targetPublicId: releasePublicId,
        requestId,
        result: 'success',
        summary: { jobId: jobPublicId, idempotencyKey, kind: 'tls_certificates' },
      })
    })
    const release = await this.find(releasePublicId)
    if (!release) throw notFound('TLS release')
    return { jobId: jobPublicId, state: jobState, release }
  }

  private toView(row: ReleaseRow): TlsCertificateReleaseView {
    return {
      id: bufferToPublicId(row.public_id),
      sequence: row.sequence_no,
      status: row.status,
      defaultCertificateId: row.default_certificate_public_id,
      certificateCount: Number(row.certificate_count),
      wireSha256: row.wire_sha256.toString('hex'),
      resource: {
        id: bufferToPublicId(row.resource_public_id),
        dataId: TLS_CERTIFICATES_DATA_ID,
        group: TLS_CERTIFICATES_GROUP,
        status: row.resource_status,
        verifiedSha256: row.verified_sha256?.toString('hex') ?? null,
        verifiedAt: row.verified_at ? mysqlDateTimeToRfc3339(row.verified_at) : null,
      },
      publication: {
        jobId: row.job_public_id ? bufferToPublicId(row.job_public_id) : null,
        state: row.job_state,
      },
      activationStatus: 'unknown',
      createdAt: mysqlDateTimeToRfc3339(row.created_at),
      publishedAt: row.published_at ? mysqlDateTimeToRfc3339(row.published_at) : null,
    }
  }
}
