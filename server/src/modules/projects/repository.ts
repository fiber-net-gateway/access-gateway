import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import { isDuplicateKeyError } from '../../database/errors.js'
import type { DatabasePool } from '../../database/types.js'
import { withTransaction } from '../../database/transaction.js'
import { conflict } from '../../shared/errors.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import { mysqlDateTimeToRfc3339 } from '../../shared/time.js'
import { AuditRepository } from '../audit/repository.js'
import type { Actor } from '../auth/model.js'
import type { ProjectView } from './model.js'

interface ProjectRow extends RowDataPacket {
  internal_id: string
  public_id: Buffer
  environment_public_id: Buffer
  name: string
  status: 'active' | 'archived'
  lock_version: string
  draft_public_id: Buffer | null
  draft_state: string | null
  current_revision_no: number | null
  draft_lock_version: string | null
  created_at: string
  updated_at: string
}

export interface ProjectIdentityRow extends RowDataPacket {
  id: string
  public_id: Buffer
  environment_id: string
  environment_public_id: Buffer
  name: string
}

function toView(row: ProjectRow): ProjectView {
  return {
    id: bufferToPublicId(row.public_id),
    environmentId: bufferToPublicId(row.environment_public_id),
    domain: row.name,
    status: row.status,
    lockVersion: row.lock_version,
    draft: row.draft_public_id
      ? {
          id: bufferToPublicId(row.draft_public_id),
          state: row.draft_state!,
          revision: row.current_revision_no!,
          lockVersion: row.draft_lock_version!,
        }
      : null,
    publishedVersion: null,
    activationStatus: 'unknown',
    createdAt: mysqlDateTimeToRfc3339(row.created_at),
    updatedAt: mysqlDateTimeToRfc3339(row.updated_at),
  }
}

const selectProjects = `
  SELECT
    p.id AS internal_id, p.public_id, e.public_id AS environment_public_id,
    p.name, p.status, p.lock_version,
    d.public_id AS draft_public_id, d.state AS draft_state,
    d.current_revision_no, d.lock_version AS draft_lock_version,
    p.created_at, p.updated_at
  FROM projects p
  INNER JOIN environments e ON e.id = p.environment_id
  LEFT JOIN drafts d ON d.project_id = p.id AND d.archived_at IS NULL
`

export class ProjectRepository {
  readonly #pool: DatabasePool
  readonly #audit: AuditRepository

  constructor(pool: DatabasePool, audit = new AuditRepository()) {
    this.#pool = pool
    this.#audit = audit
  }

  async list(actor: Actor, environmentPublicId: string): Promise<readonly ProjectView[]> {
    const permissionJoin = actor.platformAdmin
      ? ''
      : 'INNER JOIN environment_memberships m ON m.environment_id = e.id AND m.user_id = ?'
    const values = actor.platformAdmin
      ? [publicIdToBuffer(environmentPublicId)]
      : [actor.internalId, publicIdToBuffer(environmentPublicId)]
    const [rows] = await this.#pool.execute<ProjectRow[]>(
      `${selectProjects}
       ${permissionJoin}
       WHERE e.public_id = ? AND p.archived_at IS NULL
       ORDER BY p.name, p.public_id`,
      values,
    )
    return rows.map(toView)
  }

  async findView(actor: Actor, projectPublicId: string): Promise<ProjectView | null> {
    const permissionJoin = actor.platformAdmin
      ? ''
      : 'INNER JOIN environment_memberships m ON m.environment_id = e.id AND m.user_id = ?'
    const values = actor.platformAdmin
      ? [publicIdToBuffer(projectPublicId)]
      : [actor.internalId, publicIdToBuffer(projectPublicId)]
    const [rows] = await this.#pool.execute<ProjectRow[]>(
      `${selectProjects}
       ${permissionJoin}
       WHERE p.public_id = ? AND p.archived_at IS NULL`,
      values,
    )
    return rows[0] ? toView(rows[0]) : null
  }

  async findIdentity(actor: Actor, projectPublicId: string): Promise<ProjectIdentityRow | null> {
    const permissionJoin = actor.platformAdmin
      ? ''
      : 'INNER JOIN environment_memberships m ON m.environment_id = e.id AND m.user_id = ?'
    const values = actor.platformAdmin
      ? [publicIdToBuffer(projectPublicId)]
      : [actor.internalId, publicIdToBuffer(projectPublicId)]
    const [rows] = await this.#pool.execute<ProjectIdentityRow[]>(
      `SELECT p.id, p.public_id, p.environment_id, e.public_id AS environment_public_id, p.name
       FROM projects p
       INNER JOIN environments e ON e.id = p.environment_id
       ${permissionJoin}
       WHERE p.public_id = ? AND p.archived_at IS NULL`,
      values,
    )
    return rows[0] ?? null
  }

  async create(
    actor: Actor,
    environmentInternalId: string,
    environmentPublicId: string,
    domain: string,
    requestId: string,
  ): Promise<string> {
    const projectPublicId = createPublicId()
    const draftPublicId = createPublicId()
    try {
      await withTransaction(
        this.#pool,
        async (transaction) => {
          const [result] = await transaction.execute<ResultSetHeader>(
            `INSERT INTO projects
              (public_id, environment_id, name, status, created_by)
             VALUES (?, ?, ?, 'active', ?)`,
            [publicIdToBuffer(projectPublicId), environmentInternalId, domain, actor.internalId],
          )
          const projectInternalId = result.insertId.toString()
          await transaction.execute(
            'INSERT INTO project_version_counters (project_id, last_allocated_version) VALUES (?, 0)',
            [projectInternalId],
          )
          await transaction.execute(
            `INSERT INTO drafts
              (public_id, environment_id, project_id, scope_key, kind, state, title,
               created_by, updated_by)
             VALUES (?, ?, ?, ?, 'project_route', 'editing', ?, ?, ?)`,
            [
              publicIdToBuffer(draftPublicId),
              environmentInternalId,
              projectInternalId,
              `project:${projectInternalId}`,
              `${domain} route configuration`,
              actor.internalId,
              actor.internalId,
            ],
          )
          await this.#audit.append(transaction, {
            environmentInternalId,
            actorInternalId: actor.internalId,
            eventType: 'project.created',
            targetType: 'project',
            targetPublicId: projectPublicId,
            requestId,
            result: 'success',
            summary: { environmentId: environmentPublicId, domain },
          })
          await this.#audit.append(transaction, {
            environmentInternalId,
            actorInternalId: actor.internalId,
            eventType: 'draft.created',
            targetType: 'draft',
            targetPublicId: draftPublicId,
            requestId,
            result: 'success',
            summary: { projectId: projectPublicId, domain },
          })
        },
        { retryOnDeadlock: true },
      )
      return projectPublicId
    } catch (error) {
      if (isDuplicateKeyError(error)) {
        throw conflict('PROJECT_DOMAIN_EXISTS', 'A project with this domain already exists')
      }
      throw error
    }
  }
}
