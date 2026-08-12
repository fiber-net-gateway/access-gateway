import type { RowDataPacket } from 'mysql2/promise'

import type { DatabasePool } from './types.js'

export const expectedSchemaVersion = '0006_network_policies_and_certificates'

export async function currentSchemaVersion(pool: DatabasePool): Promise<string | null> {
  const [rows] = await pool.execute<(RowDataPacket & { version: string })[]>(
    'SELECT version FROM schema_migrations ORDER BY version DESC LIMIT 1',
  )
  return rows[0]?.version ?? null
}

export async function requireCurrentSchema(pool: DatabasePool): Promise<void> {
  let current: string | null
  try {
    current = await currentSchemaVersion(pool)
  } catch (error) {
    throw new Error('Database schema is not initialized; run the migration command first', {
      cause: error,
    })
  }
  if (current !== expectedSchemaVersion) {
    throw new Error(
      `Database schema ${current ?? 'none'} is not current; expected ${expectedSchemaVersion}`,
    )
  }
}
