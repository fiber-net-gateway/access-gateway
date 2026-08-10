import 'dotenv/config'

import { loadServerConfig } from '../config/env.js'
import { LocalEnvelopeDocumentCipher } from '../crypto/document-cipher.js'
import { DocumentRepository } from '../crypto/document-repository.js'
import { createDatabasePool } from '../database/pool.js'
import { requireCurrentSchema } from '../database/schema.js'
import { HttpNacosClient } from '../integrations/nacos/http.js'
import { PublicationWorker } from '../modules/publication/worker.js'

async function main(): Promise<void> {
  const config = loadServerConfig()
  if (!config.database.enabled || !config.documentEncryption.key) {
    throw new Error('Publication worker requires MySQL and document encryption')
  }
  if (!config.publication.enabled) {
    throw new Error('PUBLICATION_WORKER_ENABLED must be true')
  }

  const pool = await createDatabasePool(config.database)
  await requireCurrentSchema(pool)
  const documents = new DocumentRepository(
    new LocalEnvelopeDocumentCipher(config.documentEncryption.keyId, config.documentEncryption.key),
  )
  const nacos = new HttpNacosClient({
    timeoutMillis: config.publication.requestTimeoutMillis,
    maxResponseBytes: config.publication.maxResponseBytes,
    endpointOverride: config.publication.endpointOverride,
  })
  const worker = new PublicationWorker(pool, documents, nacos, {
    leaseMillis: config.publication.leaseMillis,
  })

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
      const worked = await worker.runOnce()
      if (!worked && !closing) {
        await new Promise<void>((resolve) => {
          wake = resolve
          const timer = setTimeout(resolve, config.publication.pollIntervalMillis)
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
  console.error(`publication worker failed: ${message}`)
  process.exitCode = 1
})
