import 'dotenv/config'

import { buildApp } from './app.js'
import { loadServerConfig } from './config/env.js'
import { createApplicationRuntime } from './runtime.js'

async function main(): Promise<void> {
  const config = loadServerConfig()
  const runtime = await createApplicationRuntime(config)
  const app = buildApp({ logger: { level: config.logLevel }, services: runtime.services })
  let closing = false

  async function shutdown(signal: NodeJS.Signals): Promise<void> {
    if (closing) {
      return
    }
    closing = true
    app.log.info({ signal }, 'shutting down console API')
    await app.close()
    await runtime.close()
  }

  process.once('SIGINT', () => void shutdown('SIGINT'))
  process.once('SIGTERM', () => void shutdown('SIGTERM'))

  try {
    const address = await app.listen({ host: config.host, port: config.port })
    app.log.info({ address }, 'console API listening')
  } catch (error) {
    app.log.fatal({ err: error }, 'failed to start console API')
    await runtime.close()
    throw error
  }
}

main().catch((error: unknown) => {
  const message = error instanceof Error ? error.message : 'unknown startup error'
  console.error(`failed to start console API: ${message}`)
  process.exitCode = 1
})
