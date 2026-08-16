import 'dotenv/config'

import { loadServerConfig } from '../config/env.js'
import { createDatabasePool } from '../database/pool.js'
import { requireCurrentSchema } from '../database/schema.js'
import { HttpActivationEvidenceClient } from '../integrations/activation-evidence/http.js'
import { ActivationCollector } from '../modules/activation/collector.js'

async function main(): Promise<void> {
  const config = loadServerConfig()
  if (!config.database.enabled) throw new Error('Activation collector requires MySQL')
  if (!config.activation.enabled || config.activation.targets.length === 0) {
    throw new Error('Activation collector requires at least one configured target')
  }

  const pool = await createDatabasePool(config.database)
  await requireCurrentSchema(pool)
  const client = new HttpActivationEvidenceClient({
    timeoutMillis: config.activation.requestTimeoutMillis,
    maxResponseBytes: config.activation.maxResponseBytes,
    maxPages: config.activation.maxPages,
    maxProjects: config.activation.maxProjects,
  })
  const collector = new ActivationCollector(pool, client, config.activation)

  let closing = false
  let wake: (() => void) | null = null
  const shutdown = (): void => {
    closing = true
    wake?.()
  }
  process.once('SIGINT', shutdown)
  process.once('SIGTERM', shutdown)

  try {
    while (!closing) {
      const collected = await collector.runOnce()
      if (collected === 0 && !closing) {
        await new Promise<void>((resolve) => {
          wake = resolve
          const timer = setTimeout(resolve, config.activation.pollIntervalMillis)
          timer.unref()
        })
        wake = null
      }
    }
  } finally {
    await pool.end()
  }
}

main().catch((error: unknown) => {
  const message = error instanceof Error ? error.message : 'unknown worker error'
  console.error(`activation collector failed: ${message}`)
  process.exitCode = 1
})
