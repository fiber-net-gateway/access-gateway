import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { DocumentRepository } from '../../crypto/document-repository.js'
import { isDuplicateKeyError } from '../../database/errors.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import { conflict, notFound } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type { ProjectIdentityRow } from '../projects/repository.js'
import type { CertificateView, ProjectCertificateBindingView } from './model.js'
import type { ParsedCertificateUpload } from './parser.js'

interface CertificateRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  environment_id: string
  display_name: string
  lifecycle_state: 'active' | 'superseded'
  fingerprint_sha256: Buffer
  serial_number: string
  subject: string
  issuer: string
  dns_names_json: string | readonly string[]
  not_before: string
  not_after: string
  key_type: string
  binding_count: string
  created_at: string
}

interface BindingRow extends CertificateRow {
  binding_public_id: Buffer
  project_public_id: Buffer
  project_name: string
  bound_at: string
}

function parseDnsNames(value: string | readonly string[]): readonly string[] {
  const parsed: unknown = typeof value === 'string' ? JSON.parse(value) : value
  if (!Array.isArray(parsed) || !parsed.every((item) => typeof item === 'string')) {
    throw new Error('Stored certificate DNS names are invalid')
  }
  return parsed
}

function certificateStatus(row: CertificateRow, now = Date.now()): CertificateView['status'] {
  if (row.lifecycle_state === 'superseded') return 'superseded'
  const notAfter = Date.parse(mysqlDateTimeToRfc3339(row.not_after))
  if (notAfter <= now) return 'expired'
  return notAfter - now <= 30 * 24 * 60 * 60 * 1_000 ? 'expiring' : 'valid'
}

function toView(row: CertificateRow): CertificateView {
  return {
    id: bufferToPublicId(row.public_id),
    name: row.display_name,
    status: certificateStatus(row),
    subject: row.subject,
    issuer: row.issuer,
    serialNumber: row.serial_number,
    fingerprintSha256: row.fingerprint_sha256.toString('hex'),
    dnsNames: parseDnsNames(row.dns_names_json),
    notBefore: mysqlDateTimeToRfc3339(row.not_before),
    notAfter: mysqlDateTimeToRfc3339(row.not_after),
    keyType: row.key_type,
    bindingCount: Number(row.binding_count),
    runtimeDeploymentStatus: 'unsupported',
    createdAt: mysqlDateTimeToRfc3339(row.created_at),
  }
}

function mysqlDateTime(value: Date): string {
  return value.toISOString().slice(0, 23).replace('T', ' ')
}

const selectCertificates = `
  SELECT
    c.id AS internal_id, c.public_id, c.environment_id,
    c.display_name, c.lifecycle_state, c.fingerprint_sha256, c.serial_number,
    c.subject, c.issuer, c.dns_names_json, c.not_before, c.not_after, c.key_type,
    (
      SELECT COUNT(*)
      FROM certificate_bindings binding_count
      WHERE binding_count.certificate_id = c.id AND binding_count.unbound_at IS NULL
    ) AS binding_count,
    c.created_at
  FROM certificates c
`

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
      : 'INNER JOIN environment_memberships m ON m.environment_id = c.environment_id AND m.user_id = ?'
    const values = actor.platformAdmin
      ? [publicIdToBuffer(environmentPublicId)]
      : [actor.internalId, publicIdToBuffer(environmentPublicId)]
    const [rows] = await this.#pool.execute<CertificateRow[]>(
      `${selectCertificates}
       INNER JOIN environments e ON e.id = c.environment_id
       ${permissionJoin}
       WHERE e.public_id = ?
       ORDER BY c.not_after, c.display_name, c.id`,
      values,
    )
    return rows.map(toView)
  }

  async findInEnvironment(
    environmentInternalId: string,
    certificatePublicId: string,
  ): Promise<(CertificateView & { internalId: string; environmentInternalId: string }) | null> {
    const [rows] = await this.#pool.execute<CertificateRow[]>(
      `${selectCertificates}
       WHERE c.environment_id = ? AND c.public_id = ?
       LIMIT 1`,
      [environmentInternalId, publicIdToBuffer(certificatePublicId)],
    )
    const row = rows[0]
    return row
      ? { ...toView(row), internalId: row.internal_id, environmentInternalId: row.environment_id }
      : null
  }

  async create(
    actor: Actor,
    environmentInternalId: string,
    parsed: ParsedCertificateUpload,
    requestId: string,
  ): Promise<string> {
    const publicId = createPublicId()
    const certificateDocument = this.#documents.encrypt(Buffer.from(parsed.certificatePem, 'utf8'))
    const privateKeyDocument = this.#documents.encrypt(Buffer.from(parsed.privateKeyPem, 'utf8'))
    try {
      await withTransaction(this.#pool, async (transaction) => {
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
        await transaction.execute<ResultSetHeader>(
          `INSERT INTO certificates
            (public_id, environment_id, display_name, lifecycle_state,
             fingerprint_sha256, serial_number, subject, issuer, dns_names_json,
             not_before, not_after, key_type, certificate_document_id,
             private_key_document_id, created_by)
           VALUES (?, ?, ?, 'active', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
          [
            publicIdToBuffer(publicId),
            environmentInternalId,
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
        await this.#audit.append(transaction, {
          environmentInternalId,
          actorInternalId: actor.internalId,
          eventType: 'certificate.created',
          targetType: 'certificate',
          targetPublicId: publicId,
          requestId,
          result: 'success',
          summary: {
            name: parsed.name,
            fingerprintSha256: parsed.fingerprintSha256.toString('hex'),
            dnsNames: parsed.dnsNames,
            notAfter: parsed.notAfter.toISOString(),
          },
        })
      })
    } catch (error) {
      if (isDuplicateKeyError(error)) {
        throw conflict('CERTIFICATE_ALREADY_EXISTS', 'This certificate is already in the inventory')
      }
      throw error
    }
    return publicId
  }

  async getBinding(project: ProjectIdentityRow): Promise<ProjectCertificateBindingView> {
    const [rows] = await this.#pool.execute<BindingRow[]>(
      `SELECT
         c.id AS internal_id, c.public_id, c.environment_id,
         c.display_name, c.lifecycle_state, c.fingerprint_sha256, c.serial_number,
         c.subject, c.issuer, c.dns_names_json, c.not_before, c.not_after, c.key_type,
         (
           SELECT COUNT(*)
           FROM certificate_bindings binding_count
           WHERE binding_count.certificate_id = c.id AND binding_count.unbound_at IS NULL
         ) AS binding_count,
         c.created_at,
         b.public_id AS binding_public_id,
         p.public_id AS project_public_id, p.name AS project_name, b.bound_at
       FROM certificate_bindings b
       INNER JOIN certificates c ON c.id = b.certificate_id
       INNER JOIN projects p ON p.id = b.project_id
       WHERE b.project_id = ? AND b.unbound_at IS NULL
       LIMIT 1`,
      [project.id],
    )
    const row = rows[0]
    return {
      projectId: bufferToPublicId(project.public_id),
      domain: project.name,
      certificate: row ? toView(row) : null,
      coverageStatus: row ? 'covered' : 'unbound',
      runtimeDeploymentStatus: 'unsupported',
      boundAt: row ? mysqlDateTimeToRfc3339(row.bound_at) : null,
    }
  }

  async bind(
    actor: Actor,
    project: ProjectIdentityRow,
    certificateInternalId: string,
    certificatePublicId: string,
    requestId: string,
  ): Promise<void> {
    await withTransaction(
      this.#pool,
      async (transaction) => {
        const [projectRows] = await transaction.execute<(RowDataPacket & { id: string })[]>(
          'SELECT id FROM projects WHERE id = ? AND archived_at IS NULL FOR UPDATE',
          [project.id],
        )
        if (!projectRows[0]) throw notFound('Project')
        await transaction.execute(
          `UPDATE certificate_bindings
           SET unbound_at = CURRENT_TIMESTAMP(6), unbound_by = ?
           WHERE project_id = ? AND unbound_at IS NULL`,
          [actor.internalId, project.id],
        )
        const bindingPublicId = createPublicId()
        await transaction.execute(
          `INSERT INTO certificate_bindings
            (public_id, project_id, certificate_id, bound_by)
           VALUES (?, ?, ?, ?)`,
          [publicIdToBuffer(bindingPublicId), project.id, certificateInternalId, actor.internalId],
        )
        await this.#audit.append(transaction, {
          environmentInternalId: project.environment_id,
          actorInternalId: actor.internalId,
          eventType: 'certificate.bound',
          targetType: 'project',
          targetPublicId: bufferToPublicId(project.public_id),
          requestId,
          result: 'success',
          summary: { certificateId: certificatePublicId, domain: project.name },
        })
      },
      { retryOnDeadlock: true },
    )
  }

  async unbind(actor: Actor, project: ProjectIdentityRow, requestId: string): Promise<void> {
    await withTransaction(this.#pool, async (transaction) => {
      const [result] = await transaction.execute<ResultSetHeader>(
        `UPDATE certificate_bindings
         SET unbound_at = CURRENT_TIMESTAMP(6), unbound_by = ?
         WHERE project_id = ? AND unbound_at IS NULL`,
        [actor.internalId, project.id],
      )
      if (result.affectedRows === 0) return
      await this.#audit.append(transaction, {
        environmentInternalId: project.environment_id,
        actorInternalId: actor.internalId,
        eventType: 'certificate.unbound',
        targetType: 'project',
        targetPublicId: bufferToPublicId(project.public_id),
        requestId,
        result: 'success',
        summary: { domain: project.name },
      })
    })
  }
}
