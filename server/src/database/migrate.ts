import { createHash } from 'node:crypto'
import { readdir, readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'

import mysql, { type RowDataPacket } from 'mysql2/promise'

import type { DatabaseConfig } from '../config/env.js'

interface AppliedMigrationRow extends RowDataPacket {
  version: string
  checksum: Buffer
}

export interface MigrationResult {
  applied: readonly string[]
  currentVersion: string | null
}

const migrationFilePattern = /^\d{4}_[a-z0-9_]+\.sql$/u
const migrationLockName = 'access-gateway:console:migration'

function digest(contents: string): Buffer {
  return createHash('sha256').update(contents).digest()
}

function migrationDirectory(): string {
  return fileURLToPath(new URL('../../migrations/', import.meta.url))
}

export async function migrateDatabase(config: DatabaseConfig): Promise<MigrationResult> {
  const connection = await mysql.createConnection({
    host: config.host,
    port: config.port,
    database: config.database,
    user: config.user,
    password: config.password,
    connectTimeout: 5_000,
    charset: 'utf8mb4',
    timezone: 'Z',
    supportBigNumbers: true,
    bigNumberStrings: true,
    dateStrings: true,
    multipleStatements: true,
    ssl: config.sslMode === 'required' ? { rejectUnauthorized: true } : undefined,
  })

  let lockAcquired = false
  try {
    await connection.query("SET time_zone = '+00:00'")
    await connection.query(
      "SET SESSION sql_mode = 'STRICT_TRANS_TABLES,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'",
    )
    const [lockRows] = await connection.query<RowDataPacket[]>(
      'SELECT GET_LOCK(?, 30) AS acquired',
      [migrationLockName],
    )
    lockAcquired = Number(lockRows[0]?.acquired) === 1
    if (!lockAcquired) {
      throw new Error('Timed out waiting for the database migration lock')
    }

    await connection.query(`
      CREATE TABLE IF NOT EXISTS schema_migrations (
        version VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
        checksum BINARY(32) NOT NULL,
        applied_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
      ) ENGINE = InnoDB
    `)

    const [appliedRows] = await connection.query<AppliedMigrationRow[]>(
      'SELECT version, checksum FROM schema_migrations ORDER BY version',
    )
    const applied = new Map(appliedRows.map((row) => [row.version, Buffer.from(row.checksum)]))
    const names = (await readdir(migrationDirectory()))
      .filter((name) => migrationFilePattern.test(name))
      .sort()
    const newlyApplied: string[] = []

    for (const name of names) {
      const version = name.slice(0, -4)
      const contents = await readFile(new URL(`../../migrations/${name}`, import.meta.url), 'utf8')
      const checksum = digest(contents)
      const previousChecksum = applied.get(version)
      if (previousChecksum) {
        if (!previousChecksum.equals(checksum)) {
          throw new Error(`Applied migration checksum does not match: ${version}`)
        }
        continue
      }

      await connection.query(contents)
      await connection.execute('INSERT INTO schema_migrations (version, checksum) VALUES (?, ?)', [
        version,
        checksum,
      ])
      newlyApplied.push(version)
    }

    const currentVersion = names.length === 0 ? null : names.at(-1)!.slice(0, -4)
    return { applied: newlyApplied, currentVersion }
  } finally {
    if (lockAcquired) {
      await connection.query('SELECT RELEASE_LOCK(?)', [migrationLockName]).catch(() => undefined)
    }
    await connection.end()
  }
}
