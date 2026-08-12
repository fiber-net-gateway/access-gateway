import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import { isDuplicateKeyError } from '../../database/errors.js'
import type { DatabasePool, SqlExecutor } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import { conflict, notFound } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type { ProjectIdentityRow } from '../projects/repository.js'
import type {
  CertificateVersionView,
  CertificateView,
  ProjectCertificateResolutionView,
} from './model.js'
import type { ParsedCertificateUpload } from './parser.js'

interface CertificateSeriesRow extends RowDataPacket {
  series_internal_id: string
  series_public_id: Buffer
  environment_id: string
  display_name: string
  managed_dns_names_json: string | readonly string[]
  lock_version: string
  series_created_at: string
  series_updated_at: string
  version_internal_id: string
  version_public_id: Buffer
  version_no: number
  lifecycle_state: 'active' | 'superseded'
  fingerprint_sha256: Buffer
  serial_number: string
  subject: string
  issuer: string
  dns_names_json: string | readonly string[]
  not_before: string
  not_after: string
  key_type: string
  version_created_at: string
  version_count: string
  matched_project_count: string
}

interface CertificateVersionRow extends RowDataPacket {
  public_id: Buffer
  version_no: number
  lifecycle_state: 'active' | 'superseded'
  fingerprint_sha256: Buffer
  serial_number: string
  subject: string
  issuer: string
  dns_names_json: string | readonly string[]
  not_before: string
  not_after: string
  key_type: string
  created_at: string
}

interface MatchedSeriesRow extends CertificateSeriesRow {
  match_kind: 'exact' | 'wildcard'
}

interface SeriesLockRow extends RowDataPacket {
  current_version_id: string | null
  current_version_no: number | null
  lock_version: string
}

function parseDnsNames(value: string | readonly string[]): readonly string[] {
  const parsed: unknown = typeof value === 'string' ? JSON.parse(value) : value
  if (!Array.isArray(parsed) || !parsed.every((item) => typeof item === 'string')) {
    throw new Error('Stored certificate DNS names are invalid')
  }
  return parsed
}

function certificateStatus(
  lifecycleState: 'active' | 'superseded',
  notAfterValue: string,
  now = Date.now(),
): CertificateVersionView['status'] {
  if (lifecycleState === 'superseded') return 'superseded'
  const notAfter = Date.parse(mysqlDateTimeToRfc3339(notAfterValue))
  if (notAfter <= now) return 'expired'
  return notAfter - now <= 30 * 24 * 60 * 60 * 1_000 ? 'expiring' : 'valid'
}

function toVersionView(row: CertificateSeriesRow): CertificateVersionView {
  return {
    id: bufferToPublicId(row.version_public_id),
    version: row.version_no,
    status: certificateStatus(row.lifecycle_state, row.not_after),
    subject: row.subject,
    issuer: row.issuer,
    serialNumber: row.serial_number,
    fingerprintSha256: row.fingerprint_sha256.toString('hex'),
    dnsNames: parseDnsNames(row.dns_names_json),
    notBefore: mysqlDateTimeToRfc3339(row.not_before),
    notAfter: mysqlDateTimeToRfc3339(row.not_after),
    keyType: row.key_type,
    createdAt: mysqlDateTimeToRfc3339(row.version_created_at),
  }
}

function toHistoricalVersionView(row: CertificateVersionRow): CertificateVersionView {
  return {
    id: bufferToPublicId(row.public_id),
    version: row.version_no,
    status: certificateStatus(row.lifecycle_state, row.not_after),
    subject: row.subject,
    issuer: row.issuer,
    serialNumber: row.serial_number,
    fingerprintSha256: row.fingerprint_sha256.toString('hex'),
    dnsNames: parseDnsNames(row.dns_names_json),
    notBefore: mysqlDateTimeToRfc3339(row.not_before),
    notAfter: mysqlDateTimeToRfc3339(row.not_after),
    keyType: row.key_type,
    createdAt: mysqlDateTimeToRfc3339(row.created_at),
  }
}

function toView(row: CertificateSeriesRow): CertificateView {
  return {
    id: bufferToPublicId(row.series_public_id),
    name: row.display_name,
    lockVersion: row.lock_version,
    managedDnsNames: parseDnsNames(row.managed_dns_names_json),
    currentVersion: toVersionView(row),
    versionCount: Number(row.version_count),
    matchedProjectCount: Number(row.matched_project_count),
    runtimeDeploymentStatus: 'unsupported',
    createdAt: mysqlDateTimeToRfc3339(row.series_created_at),
    updatedAt: mysqlDateTimeToRfc3339(row.series_updated_at),
  }
}

function mysqlDateTime(value: Date): string {
  return value.toISOString().slice(0, 23).replace('T', ' ')
}

function wildcardDotCount(name: string): number | null {
  if (!name.startsWith('*.')) return null
  return [...name].filter((character) => character === '.').length
}

const projectNameMatch = `
  (
    (certificate_name.match_kind = 'exact' AND project.name = certificate_name.match_value)
    OR
    (
      certificate_name.match_kind = 'wildcard'
      AND project.name LIKE CONCAT('%.', certificate_name.match_value)
      AND LENGTH(project.name) - LENGTH(REPLACE(project.name, '.', '')) =
          certificate_name.wildcard_dot_count
    )
  )
`

function selectSeries(additionalColumns = ''): string {
  return `
  SELECT
    series_record.id AS series_internal_id,
    series_record.public_id AS series_public_id,
    series_record.environment_id,
    series_record.display_name,
    series_record.managed_dns_names_json,
    series_record.lock_version,
    series_record.created_at AS series_created_at,
    series_record.updated_at AS series_updated_at,
    current_version.id AS version_internal_id,
    current_version.public_id AS version_public_id,
    current_version.version_no,
    current_version.lifecycle_state,
    current_version.fingerprint_sha256,
    current_version.serial_number,
    current_version.subject,
    current_version.issuer,
    current_version.dns_names_json,
    current_version.not_before,
    current_version.not_after,
    current_version.key_type,
    current_version.created_at AS version_created_at,
    (
      SELECT COUNT(*)
      FROM certificates version_count
      WHERE version_count.series_id = series_record.id
    ) AS version_count,
    (
      SELECT COUNT(DISTINCT project.id)
      FROM certificate_dns_names certificate_name
      INNER JOIN projects project
        ON project.environment_id = series_record.environment_id
        AND project.archived_at IS NULL
        AND ${projectNameMatch}
      WHERE certificate_name.certificate_series_id = series_record.id
    ) AS matched_project_count${additionalColumns}
  FROM certificate_series series_record
  INNER JOIN certificates current_version ON current_version.id = series_record.current_version_id
`
}

export class CertificateRepository {
  readonly #pool: DatabasePool
  readonly #documents: DocumentRepository
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, documents: DocumentRepository, audit = new AuditRepository()) {
    this.#pool = pool
    this.#documents = documents
    this.#audit = audit
  }

  async list(actor: Actor, environmentPublicId: string): Promise<readonly CertificateView[]> {
    const permissionJoin = actor.platformAdmin
      ? ''
      : `INNER JOIN environment_memberships membership
           ON membership.environment_id = series_record.environment_id
          AND membership.user_id = ?`
    const values = actor.platformAdmin
      ? [publicIdToBuffer(environmentPublicId)]
      : [actor.internalId, publicIdToBuffer(environmentPublicId)]
    const [rows] = await this.#pool.execute<CertificateSeriesRow[]>(
      `${selectSeries()}
       INNER JOIN environments environment_record
         ON environment_record.id = series_record.environment_id
       ${permissionJoin}
       WHERE environment_record.public_id = ? AND series_record.archived_at IS NULL
       ORDER BY series_record.display_name, series_record.id`,
      values,
    )
    return rows.map(toView)
  }

  async findInEnvironment(
    environmentInternalId: string,
    certificatePublicId: string,
  ): Promise<
    | (CertificateView & {
        internalId: string
        environmentInternalId: string
      })
    | null
  > {
    const [rows] = await this.#pool.execute<CertificateSeriesRow[]>(
      `${selectSeries()}
       WHERE series_record.environment_id = ?
         AND series_record.public_id = ?
         AND series_record.archived_at IS NULL
       LIMIT 1`,
      [environmentInternalId, publicIdToBuffer(certificatePublicId)],
    )
    const row = rows[0]
    return row
      ? {
          ...toView(row),
          internalId: row.series_internal_id,
          environmentInternalId: row.environment_id,
        }
      : null
  }

  async listVersions(seriesInternalId: string): Promise<readonly CertificateVersionView[]> {
    const [rows] = await this.#pool.execute<CertificateVersionRow[]>(
      `SELECT
         public_id, version_no, lifecycle_state, fingerprint_sha256, serial_number,
         subject, issuer, dns_names_json, not_before, not_after, key_type, created_at
       FROM certificates
       WHERE series_id = ?
       ORDER BY version_no DESC`,
      [seriesInternalId],
    )
    return rows.map(toHistoricalVersionView)
  }

  async create(
    actor: Actor,
    environmentInternalId: string,
    parsed: ParsedCertificateUpload,
    requestId: string,
  ): Promise<string> {
    const seriesPublicId = createPublicId()
    const versionPublicId = createPublicId()
    const certificateDocument = this.#documents.encrypt(Buffer.from(parsed.certificatePem, 'utf8'))
    const privateKeyDocument = this.#documents.encrypt(Buffer.from(parsed.privateKeyPem, 'utf8'))
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          await this.lockEnvironment(transaction, environmentInternalId)
          await this.rejectManagedNameConflicts(transaction, environmentInternalId, parsed.dnsNames)
          const [seriesResult] = await transaction.execute<ResultSetHeader>(
            `INSERT INTO certificate_series
              (public_id, environment_id, display_name, managed_dns_names_json,
               current_version_id, created_by)
             VALUES (?, ?, ?, ?, NULL, ?)`,
            [
              publicIdToBuffer(seriesPublicId),
              environmentInternalId,
              parsed.name,
              JSON.stringify(parsed.dnsNames),
              actor.internalId,
            ],
          )
          const seriesInternalId = seriesResult.insertId.toString()
          await this.insertManagedNames(
            transaction,
            environmentInternalId,
            seriesInternalId,
            parsed.dnsNames,
          )
          const versionInternalId = await this.insertVersion(
            transaction,
            actor,
            environmentInternalId,
            seriesInternalId,
            versionPublicId,
            1,
            parsed,
            certificateDocument,
            privateKeyDocument,
          )
          await transaction.execute(
            'UPDATE certificate_series SET current_version_id = ? WHERE id = ?',
            [versionInternalId, seriesInternalId],
          )
          await this.#audit.append(transaction, {
            environmentInternalId,
            actorInternalId: actor.internalId,
            eventType: 'certificate.created',
            targetType: 'certificate',
            targetPublicId: seriesPublicId,
            requestId,
            result: 'success',
            summary: {
              name: parsed.name,
              version: 1,
              versionId: versionPublicId,
              fingerprintSha256: parsed.fingerprintSha256.toString('hex'),
              managedDnsNames: parsed.dnsNames,
              notAfter: parsed.notAfter.toISOString(),
            },
          })
        },
        { retryOnDeadlock: true },
      )
    } catch (error) {
      if (isDuplicateKeyError(error)) {
        throw conflict('CERTIFICATE_ALREADY_EXISTS', 'This certificate version already exists')
      }
      throw error
    }
    return seriesPublicId
  }

  async createVersion(
    actor: Actor,
    seriesInternalId: string,
    environmentInternalId: string,
    seriesPublicId: string,
    displayName: string,
    expectedLockVersion: string,
    parsed: ParsedCertificateUpload,
    requestId: string,
  ): Promise<void> {
    const versionPublicId = createPublicId()
    const certificateDocument = this.#documents.encrypt(Buffer.from(parsed.certificatePem, 'utf8'))
    const privateKeyDocument = this.#documents.encrypt(Buffer.from(parsed.privateKeyPem, 'utf8'))
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          const [seriesRows] = await transaction.execute<SeriesLockRow[]>(
            `SELECT
               series_record.current_version_id, series_record.lock_version,
               current_version.version_no AS current_version_no
             FROM certificate_series series_record
             LEFT JOIN certificates current_version
               ON current_version.id = series_record.current_version_id
             WHERE series_record.id = ?
               AND series_record.environment_id = ?
               AND series_record.archived_at IS NULL
             FOR UPDATE`,
            [seriesInternalId, environmentInternalId],
          )
          const series = seriesRows[0]
          if (!series || series.current_version_id === null || series.current_version_no === null) {
            throw notFound('Certificate')
          }
          if (series.lock_version !== expectedLockVersion) {
            throw conflict(
              'CERTIFICATE_VERSION_CONFLICT',
              'The logical certificate changed; reload it before creating another version',
            )
          }
          const versionNo = series.current_version_no + 1
          const versionInternalId = await this.insertVersion(
            transaction,
            actor,
            environmentInternalId,
            seriesInternalId,
            versionPublicId,
            versionNo,
            { ...parsed, name: displayName },
            certificateDocument,
            privateKeyDocument,
          )
          await transaction.execute(
            `UPDATE certificates
             SET lifecycle_state = 'superseded', superseded_at = CURRENT_TIMESTAMP(6)
             WHERE id = ?`,
            [series.current_version_id],
          )
          await transaction.execute(
            `UPDATE certificate_series
             SET current_version_id = ?, lock_version = lock_version + 1,
                 updated_at = CURRENT_TIMESTAMP(6)
             WHERE id = ?`,
            [versionInternalId, seriesInternalId],
          )
          await this.#audit.append(transaction, {
            environmentInternalId,
            actorInternalId: actor.internalId,
            eventType: 'certificate.version.created',
            targetType: 'certificate',
            targetPublicId: seriesPublicId,
            requestId,
            result: 'success',
            summary: {
              version: versionNo,
              versionId: versionPublicId,
              fingerprintSha256: parsed.fingerprintSha256.toString('hex'),
              dnsNames: parsed.dnsNames,
              notAfter: parsed.notAfter.toISOString(),
            },
          })
        },
        { retryOnDeadlock: true },
      )
    } catch (error) {
      if (isDuplicateKeyError(error)) {
        throw conflict('CERTIFICATE_ALREADY_EXISTS', 'This certificate version already exists')
      }
      throw error
    }
  }

  async resolveProject(project: ProjectIdentityRow): Promise<ProjectCertificateResolutionView> {
    const [rows] = await this.#pool.execute<MatchedSeriesRow[]>(
      `${selectSeries(', matched_name.match_kind AS match_kind')}
       INNER JOIN certificate_dns_names matched_name
         ON matched_name.certificate_series_id = series_record.id
       WHERE series_record.environment_id = ?
         AND series_record.archived_at IS NULL
         AND (
           (matched_name.match_kind = 'exact' AND matched_name.match_value = ?)
           OR
           (
             matched_name.match_kind = 'wildcard'
             AND ? LIKE CONCAT('%.', matched_name.match_value)
             AND LENGTH(?) - LENGTH(REPLACE(?, '.', '')) = matched_name.wildcard_dot_count
           )
         )
       ORDER BY
         CASE matched_name.match_kind WHEN 'exact' THEN 0 ELSE 1 END,
         series_record.id`,
      [project.environment_id, project.name, project.name, project.name, project.name],
    )
    const preferredKind = rows.some((row) => row.match_kind === 'exact') ? 'exact' : 'wildcard'
    const seen = new Set<string>()
    const matches = rows
      .filter((row) => row.match_kind === preferredKind)
      .filter((row) => {
        if (seen.has(row.series_internal_id)) return false
        seen.add(row.series_internal_id)
        return true
      })
      .map(toView)
    const resolutionStatus =
      matches.length === 0 ? 'uncovered' : matches.length === 1 ? 'matched' : 'conflict'
    return {
      projectId: bufferToPublicId(project.public_id),
      domain: project.name,
      resolutionStatus,
      certificate: matches.length === 1 ? matches[0]! : null,
      matches,
      runtimeDeploymentStatus: 'unsupported',
    }
  }

  private async lockEnvironment(
    transaction: SqlExecutor,
    environmentInternalId: string,
  ): Promise<void> {
    const [rows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
      'SELECT id FROM environments WHERE id = ? FOR UPDATE',
      [environmentInternalId],
    )
    if (!rows[0]) throw notFound('Deployment workspace')
  }

  private async rejectManagedNameConflicts(
    transaction: SqlExecutor,
    environmentInternalId: string,
    dnsNames: readonly string[],
  ): Promise<void> {
    const placeholders = dnsNames.map(() => '?').join(', ')
    const [rows] = await transaction.execute<(RowDataPacket & { dns_name: string })[]>(
      `SELECT dns_name
       FROM certificate_dns_names
       WHERE environment_id = ? AND dns_name IN (${placeholders})
       ORDER BY dns_name`,
      [environmentInternalId, ...dnsNames],
    )
    if (rows.length > 0) {
      throw conflict(
        'CERTIFICATE_DNS_NAME_CONFLICT',
        `DNS selector ${rows[0]!.dns_name} is already managed by another certificate`,
      )
    }
  }

  private async insertManagedNames(
    transaction: SqlExecutor,
    environmentInternalId: string,
    seriesInternalId: string,
    dnsNames: readonly string[],
  ): Promise<void> {
    for (const dnsName of dnsNames) {
      const isWildcard = dnsName.startsWith('*.')
      await transaction.execute(
        `INSERT INTO certificate_dns_names
          (environment_id, certificate_series_id, dns_name, match_kind, match_value,
           wildcard_dot_count)
         VALUES (?, ?, ?, ?, ?, ?)`,
        [
          environmentInternalId,
          seriesInternalId,
          dnsName,
          isWildcard ? 'wildcard' : 'exact',
          isWildcard ? dnsName.slice(2) : dnsName,
          wildcardDotCount(dnsName),
        ],
      )
    }
  }

  private async insertVersion(
    transaction: SqlExecutor,
    actor: Actor,
    environmentInternalId: string,
    seriesInternalId: string,
    versionPublicId: string,
    versionNo: number,
    parsed: ParsedCertificateUpload,
    certificateDocument: ReturnType<DocumentRepository['encrypt']>,
    privateKeyDocument: ReturnType<DocumentRepository['encrypt']>,
  ): Promise<string> {
    const certificate = await this.#documents.insert(transaction, {
      environmentInternalId,
      purpose: 'certificate_chain',
      contentType: 'application/x-pem-file',
      schemaVersion: 1,
      encrypted: certificateDocument,
    })
    const privateKey = await this.#documents.insert(transaction, {
      environmentInternalId,
      purpose: 'certificate_private_key',
      contentType: 'application/x-pem-file',
      schemaVersion: 1,
      encrypted: privateKeyDocument,
    })
    const [result] = await transaction.execute<ResultSetHeader>(
      `INSERT INTO certificates
        (public_id, environment_id, series_id, version_no, display_name, lifecycle_state,
         fingerprint_sha256, serial_number, subject, issuer, dns_names_json,
         not_before, not_after, key_type, certificate_document_id,
         private_key_document_id, created_by)
       VALUES (?, ?, ?, ?, ?, 'active', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      [
        publicIdToBuffer(versionPublicId),
        environmentInternalId,
        seriesInternalId,
        versionNo,
        parsed.name,
        parsed.fingerprintSha256,
        parsed.serialNumber,
        parsed.subject,
        parsed.issuer,
        JSON.stringify(parsed.dnsNames),
        mysqlDateTime(parsed.notBefore),
        mysqlDateTime(parsed.notAfter),
        parsed.keyType,
        certificate.internalId,
        privateKey.internalId,
        actor.internalId,
      ],
    )
    return result.insertId.toString()
  }
}
