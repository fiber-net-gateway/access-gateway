import type { RowDataPacket } from 'mysql2/promise'

import type { DatabasePool } from '../../database/types.js'
import { bufferToPublicId, createPublicId, publicIdToBuffer } from '../../shared/ids.js'
import type { Actor } from './model.js'

interface UserRow extends RowDataPacket {
  id: string
  public_id: Buffer
  subject: string
  display_name: string
  is_platform_admin: number
}

export class AuthRepository {
  readonly #pool: DatabasePool

  constructor(pool: DatabasePool) {
    this.#pool = pool
  }

  async ensureDevelopmentActor(subject: string, displayName: string): Promise<Actor> {
    await this.#pool.execute(
      `INSERT INTO users
        (public_id, subject, display_name, status, is_platform_admin)
       VALUES (?, ?, ?, 'active', TRUE)
       ON DUPLICATE KEY UPDATE
        display_name = VALUES(display_name),
        status = 'active',
        is_platform_admin = TRUE,
        updated_at = CURRENT_TIMESTAMP(6)`,
      [publicIdToBuffer(createPublicId()), subject, displayName],
    )
    const [rows] = await this.#pool.execute<UserRow[]>(
      `SELECT id, public_id, subject, display_name, is_platform_admin
       FROM users
       WHERE subject = ? AND status = 'active'`,
      [subject],
    )
    const row = rows[0]
    if (!row) {
      throw new Error('Failed to load the development actor')
    }
    return {
      internalId: row.id,
      publicId: bufferToPublicId(row.public_id),
      subject: row.subject,
      displayName: row.display_name,
      platformAdmin: row.is_platform_admin === 1,
    }
  }
}
