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
import type { CertificateVersionView, CertificateView } from './model.js'
import type { ParsedCertificateUpload } from './parser.js'

interface CertificateSeriesRow extends RowDataPacket {
  series_internal_id: string
  series_public_id: Buffer
  environment_id: string
  display_name: string
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

interface SeriesLockRow extends RowDataPacket {
  current_version_id: string | null
  current_version_no: number | null
  current_dns_names_json: string | readonly string[] | null
  lock_version: string
}

interface SanConflictRow extends RowDataPacket {
  dns_name: string
  certificate_public_id: Buffer
  certificate_name: string
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
    currentVersion: toVersionView(row),
    versionCount: Number(row.version_count),
    runtimeDeploymentStatus: 'activation_unknown',
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

export function certificateSniCoverageChanges(
  currentNames: readonly string[],
  nextNames: readonly string[],
): { added: readonly string[]; removed: readonly string[] } {
  const current = new Set(currentNames)
  const next = new Set(nextNames)
  return {
    added: nextNames.filter((name) => !current.has(name)),
    removed: currentNames.filter((name) => !next.has(name)),
  }
}

function selectSeries(): string {
  return `
  SELECT
    series_record.id AS series_internal_id,
    series_record.public_id AS series_public_id,
    series_record.environment_id,
    series_record.display_name,
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
    ) AS version_count
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
          await this.rejectSanConflicts(transaction, environmentInternalId, parsed.dnsNames)
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
          await this.replaceSanSelectors(
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
              dnsNames: parsed.dnsNames,
              sniNames: parsed.dnsNames,
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
    confirmSniCoverageChange: boolean,
    requestId: string,
  ): Promise<void> {
    const versionPublicId = createPublicId()
    const certificateDocument = this.#documents.encrypt(Buffer.from(parsed.certificatePem, 'utf8'))
    const privateKeyDocument = this.#documents.encrypt(Buffer.from(parsed.privateKeyPem, 'utf8'))
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          await this.lockEnvironment(transaction, environmentInternalId)
          const [seriesRows] = await transaction.execute<SeriesLockRow[]>(
            `SELECT
               series_record.current_version_id, series_record.lock_version,
               current_version.version_no AS current_version_no,
               current_version.dns_names_json AS current_dns_names_json
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
          if (
            !series ||
            series.current_version_id === null ||
            series.current_version_no === null ||
            series.current_dns_names_json === null
          ) {
            throw notFound('Certificate')
          }
          if (series.lock_version !== expectedLockVersion) {
            throw conflict(
              'CERTIFICATE_VERSION_CONFLICT',
              'The logical certificate changed; reload it before creating another version',
            )
          }
          const changes = certificateSniCoverageChanges(
            parseDnsNames(series.current_dns_names_json),
            parsed.dnsNames,
          )
          if (
            (changes.added.length > 0 || changes.removed.length > 0) &&
            !confirmSniCoverageChange
          ) {
            throw conflict(
              'CERTIFICATE_SNI_COVERAGE_CONFIRMATION_REQUIRED',
              'The certificate DNS SAN coverage changed; confirm the SNI coverage change',
              [
                ...changes.added.map((dnsName) => ({
                  path: 'confirmSniCoverageChange',
                  code: 'SNI_NAME_ADDED',
                  message: `${dnsName} will start selecting this certificate`,
                })),
                ...changes.removed.map((dnsName) => ({
                  path: 'confirmSniCoverageChange',
                  code: 'SNI_NAME_REMOVED',
                  message: `${dnsName} will stop selecting this certificate`,
                })),
              ],
            )
          }
          await this.rejectSanConflicts(
            transaction,
            environmentInternalId,
            parsed.dnsNames,
            seriesInternalId,
          )
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
          await this.replaceSanSelectors(
            transaction,
            environmentInternalId,
            seriesInternalId,
            parsed.dnsNames,
          )
          await transaction.execute(
            `UPDATE certificates
             SET lifecycle_state = 'superseded', superseded_at = CURRENT_TIMESTAMP(6)
             WHERE id = ?`,
            [series.current_version_id],
          )
          await transaction.execute(
            `UPDATE certificate_series
             SET current_version_id = ?, managed_dns_names_json = ?,
                 lock_version = lock_version + 1,
                 updated_at = CURRENT_TIMESTAMP(6)
             WHERE id = ?`,
            [versionInternalId, JSON.stringify(parsed.dnsNames), seriesInternalId],
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
              addedSniNames: changes.added,
              removedSniNames: changes.removed,
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

  private async rejectSanConflicts(
    transaction: SqlExecutor,
    environmentInternalId: string,
    dnsNames: readonly string[],
    excludedSeriesInternalId?: string,
  ): Promise<void> {
    const placeholders = dnsNames.map(() => '?').join(', ')
    const exclusion = excludedSeriesInternalId ? 'AND selector.certificate_series_id <> ?' : ''
    const values = excludedSeriesInternalId
      ? [environmentInternalId, ...dnsNames, excludedSeriesInternalId]
      : [environmentInternalId, ...dnsNames]
    const [rows] = await transaction.execute<SanConflictRow[]>(
      `SELECT
         selector.dns_name,
         series_record.public_id AS certificate_public_id,
         series_record.display_name AS certificate_name
       FROM certificate_san_selectors selector
       INNER JOIN certificate_series series_record
         ON series_record.id = selector.certificate_series_id
       WHERE selector.environment_id = ?
         AND selector.dns_name IN (${placeholders})
         ${exclusion}
       ORDER BY selector.dns_name, selector.id`,
      values,
    )
    if (rows.length === 0) return
    throw conflict(
      'CERTIFICATE_SNI_NAME_CONFLICT',
      'One or more DNS SAN selectors are already owned by another logical certificate',
      rows.map((row) => ({
        path: 'certificatePem',
        code: 'SNI_NAME_CONFLICT',
        message: `${row.dns_name} is already provided by ${row.certificate_name} (${bufferToPublicId(row.certificate_public_id)})`,
      })),
    )
  }

  private async replaceSanSelectors(
    transaction: SqlExecutor,
    environmentInternalId: string,
    certificateSeriesInternalId: string,
    dnsNames: readonly string[],
  ): Promise<void> {
    await transaction.execute(
      'DELETE FROM certificate_san_selectors WHERE certificate_series_id = ?',
      [certificateSeriesInternalId],
    )
    for (const dnsName of dnsNames) {
      const wildcard = dnsName.startsWith('*.')
      await transaction.execute(
        `INSERT INTO certificate_san_selectors
          (environment_id, certificate_series_id, dns_name, match_kind,
           match_value, wildcard_dot_count)
         VALUES (?, ?, ?, ?, ?, ?)`,
        [
          environmentInternalId,
          certificateSeriesInternalId,
          dnsName,
          wildcard ? 'wildcard' : 'exact',
          wildcard ? dnsName.slice(2) : dnsName,
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
