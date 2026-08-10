import type { RowDataPacket } from 'mysql2/promise'

import { isDuplicateKeyError } from '../../database/errors.js'
import type { DatabasePool, SqlExecutor } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import { conflict } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type { CreateEnvironmentInput, EnvironmentTier, EnvironmentView } from './model.js'

interface EnvironmentRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  code: string
  name: string
  tier: EnvironmentTier
  status: 'active' | 'disabled'
  nacos_endpoint: string
  nacos_namespace: string
  nacos_tenant: string
  nacos_secret_ref_id: string | null
  projects_data_id: string
  route_data_id_prefix: string
  route_group: string
  gray_data_id: string
  gray_group: string
  naming_group: string
  zone: string
  protection_policy: string | Record<string, unknown>
  lock_version: string
  created_at: string
  updated_at: string
}

function parseJsonObject(value: string | Record<string, unknown>): Record<string, unknown> {
  const parsed: unknown = typeof value === 'string' ? JSON.parse(value) : value
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new Error('Stored environment protection policy is invalid')
  }
  return parsed as Record<string, unknown>
}

function toView(row: EnvironmentRow): EnvironmentView {
  return {
    id: bufferToPublicId(row.public_id),
    code: row.code,
    name: row.name,
    tier: row.tier,
    status: row.status,
    nacos: {
      endpoint: row.nacos_endpoint,
      namespace: row.nacos_namespace,
      tenant: row.nacos_tenant,
      credentialConfigured: row.nacos_secret_ref_id !== null,
    },
    dataIds: {
      projects: row.projects_data_id,
      routePrefix: row.route_data_id_prefix,
      routeGroup: row.route_group,
      gray: row.gray_data_id,
      grayGroup: row.gray_group,
      namingGroup: row.naming_group,
    },
    zone: row.zone,
    protectionPolicy: parseJsonObject(row.protection_policy),
    lockVersion: row.lock_version,
    createdAt: mysqlDateTimeToRfc3339(row.created_at),
    updatedAt: mysqlDateTimeToRfc3339(row.updated_at),
  }
}

const selectEnvironment = `
  SELECT
    e.id AS internal_id, e.public_id, e.code, e.name, e.tier, e.status,
    e.nacos_endpoint, e.nacos_namespace, e.nacos_tenant, e.nacos_secret_ref_id,
    e.projects_data_id, e.route_data_id_prefix, e.route_group,
    e.gray_data_id, e.gray_group, e.naming_group, e.zone,
    e.protection_policy, e.lock_version, e.created_at, e.updated_at
  FROM environments e
`

export class EnvironmentRepository {
  readonly #pool: DatabasePool
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, audit = new AuditRepository()) {
    this.#pool = pool
    this.#audit = audit
  }

  async list(actor: Actor): Promise<readonly EnvironmentView[]> {
    const [rows] = actor.platformAdmin
      ? await this.#pool.execute<EnvironmentRow[]>(
          `${selectEnvironment} ORDER BY e.code, e.public_id`,
        )
      : await this.#pool.execute<EnvironmentRow[]>(
          `${selectEnvironment}
           INNER JOIN environment_memberships m ON m.environment_id = e.id
           WHERE m.user_id = ?
           ORDER BY e.code, e.public_id`,
          [actor.internalId],
        )
    return rows.map(toView)
  }

  async findWorkspace(actor: Actor): Promise<EnvironmentView | null> {
    const [rows] = actor.platformAdmin
      ? await this.#pool.execute<EnvironmentRow[]>(`${selectEnvironment} ORDER BY e.id LIMIT 1`)
      : await this.#pool.execute<EnvironmentRow[]>(
          `${selectEnvironment}
           INNER JOIN environment_memberships m ON m.environment_id = e.id
           WHERE m.user_id = ?
           ORDER BY e.id
           LIMIT 1`,
          [actor.internalId],
        )
    return rows[0] ? toView(rows[0]) : null
  }

  async findAccessibleByPublicId(actor: Actor, publicId: string): Promise<EnvironmentView | null> {
    const [rows] = actor.platformAdmin
      ? await this.#pool.execute<EnvironmentRow[]>(`${selectEnvironment} WHERE e.public_id = ?`, [
          publicIdToBuffer(publicId),
        ])
      : await this.#pool.execute<EnvironmentRow[]>(
          `${selectEnvironment}
           INNER JOIN environment_memberships m ON m.environment_id = e.id
           WHERE e.public_id = ? AND m.user_id = ?`,
          [publicIdToBuffer(publicId), actor.internalId],
        )
    return rows[0] ? toView(rows[0]) : null
  }

  async internalIdForActor(actor: Actor, publicId: string): Promise<string | null> {
    const [rows] = actor.platformAdmin
      ? await this.#pool.execute<(RowDataPacket & { id: string })[]>(
          'SELECT id FROM environments WHERE public_id = ?',
          [publicIdToBuffer(publicId)],
        )
      : await this.#pool.execute<(RowDataPacket & { id: string })[]>(
          `SELECT e.id
           FROM environments e
           INNER JOIN environment_memberships m ON m.environment_id = e.id
           WHERE e.public_id = ? AND m.user_id = ?`,
          [publicIdToBuffer(publicId), actor.internalId],
        )
    return rows[0]?.id ?? null
  }

  async create(
    actor: Actor,
    input: CreateEnvironmentInput,
    requestId: string,
  ): Promise<EnvironmentView> {
    const publicId = createPublicId()
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          const [existing] = await transaction.execute<(RowDataPacket & { id: string })[]>(
            'SELECT id FROM environments ORDER BY id LIMIT 1 FOR UPDATE',
          )
          if (existing.length !== 0) {
            throw conflict(
              'FIXED_WORKSPACE_EXISTS',
              'This deployment already has its fixed workspace',
            )
          }
          const [result] = await transaction.execute<import('mysql2/promise').ResultSetHeader>(
            `INSERT INTO environments
              (public_id, code, name, tier, status, nacos_endpoint, nacos_namespace,
               nacos_tenant, projects_data_id, route_data_id_prefix, route_group,
               gray_data_id, gray_group, naming_group, zone, protection_policy,
               created_by, updated_by)
             VALUES (?, ?, ?, ?, 'active', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
            [
              publicIdToBuffer(publicId),
              input.code,
              input.name,
              input.tier,
              input.nacosEndpoint,
              input.nacosNamespace ?? 'public',
              input.nacosTenant ?? '',
              input.dataIds?.projects ?? 'ploto.unified-access.projects',
              input.dataIds?.routePrefix ?? 'ploto.unified-access.route.',
              input.dataIds?.routeGroup ?? 'ACCESS-SERVER',
              input.dataIds?.gray ?? 'ploto.unified-access.gray-match',
              input.dataIds?.grayGroup ?? 'DEFAULT_GROUP',
              input.dataIds?.namingGroup ?? 'DEFAULT_GROUP',
              input.zone ?? '',
              JSON.stringify(input.protectionPolicy ?? {}),
              actor.internalId,
              actor.internalId,
            ],
          )
          const environmentInternalId = result.insertId.toString()
          await transaction.execute(
            `INSERT INTO environment_memberships
              (environment_id, user_id, role, created_by)
             VALUES (?, ?, 'admin', ?)`,
            [environmentInternalId, actor.internalId, actor.internalId],
          )
          await this.#audit.append(transaction, {
            environmentInternalId,
            actorInternalId: actor.internalId,
            eventType: 'environment.created',
            targetType: 'environment',
            targetPublicId: publicId,
            requestId,
            result: 'success',
            summary: { code: input.code, tier: input.tier },
          })
        },
        { retryOnDeadlock: true },
      )
    } catch (error) {
      if (isDuplicateKeyError(error)) {
        throw conflict('ENVIRONMENT_CODE_EXISTS', 'An environment with this code already exists')
      }
      throw error
    }

    const created = await this.findAccessibleByPublicId(actor, publicId)
    if (!created) {
      throw new Error('Created environment could not be reloaded')
    }
    return created
  }

  async role(actor: Actor, environmentPublicId: string): Promise<string | null> {
    if (actor.platformAdmin) {
      return 'admin'
    }
    const [rows] = await this.#pool.execute<(RowDataPacket & { role: string })[]>(
      `SELECT m.role
       FROM environment_memberships m
       INNER JOIN environments e ON e.id = m.environment_id
       WHERE e.public_id = ? AND m.user_id = ?`,
      [publicIdToBuffer(environmentPublicId), actor.internalId],
    )
    return rows[0]?.role ?? null
  }

  async findInternalByPublicId(executor: SqlExecutor, publicId: string): Promise<string | null> {
    const [rows] = await executor.execute<(RowDataPacket & { id: string })[]>(
      'SELECT id FROM environments WHERE public_id = ?',
      [publicIdToBuffer(publicId)],
    )
    return rows[0]?.id ?? null
  }
}
