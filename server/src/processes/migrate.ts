import 'dotenv/config'

import { loadServerConfig } from '../config/env.js'
import { migrateDatabase } from '../database/migrate.js'

const config = loadServerConfig()
if (!config.database.enabled) {
  throw new Error('MYSQL_ENABLED=true is required to run database migrations')
}

const result = await migrateDatabase(config.database)
process.stdout.write(
  `${JSON.stringify({ applied: result.applied, currentVersion: result.currentVersion })}\n`,
)
