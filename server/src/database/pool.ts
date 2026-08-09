import mysql, { type PoolOptions } from 'mysql2/promise'

import type { DatabaseConfig } from '../config/env.js'
import type { DatabasePool } from './types.js'

function poolOptions(config: DatabaseConfig): PoolOptions {
  return {
    host: config.host,
    port: config.port,
    database: config.database,
    user: config.user,
    password: config.password,
    connectionLimit: config.connectionLimit,
    maxIdle: config.connectionLimit,
    idleTimeout: 60_000,
    waitForConnections: true,
    queueLimit: config.connectionLimit * 4,
    connectTimeout: 5_000,
    enableKeepAlive: true,
    keepAliveInitialDelay: 0,
    charset: 'utf8mb4',
    timezone: 'Z',
    supportBigNumbers: true,
    bigNumberStrings: true,
    dateStrings: true,
    decimalNumbers: false,
    ssl: config.sslMode === 'required' ? { rejectUnauthorized: true } : undefined,
  }
}

export async function createDatabasePool(config: DatabaseConfig): Promise<DatabasePool> {
  const pool = mysql.createPool(poolOptions(config))
  pool.pool.on('connection', (connection) => {
    connection.query("SET SESSION time_zone = '+00:00'", (error) => {
      if (error) connection.destroy()
    })
    connection.query(
      "SET SESSION sql_mode = 'STRICT_TRANS_TABLES,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'",
      (error) => {
        if (error) connection.destroy()
      },
    )
  })
  try {
    await pool.execute('SELECT 1')
  } catch (error) {
    await pool.end()
    throw error
  }
  return pool
}

export async function checkDatabase(pool: DatabasePool): Promise<void> {
  await pool.execute('SELECT 1')
}
