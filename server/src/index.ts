import 'dotenv/config'

import { buildApp } from './app.js'
import { loadServerConfig } from './config/env.js'

const config = loadServerConfig()
const app = buildApp({ logger: { level: config.logLevel } })
let closing = false

async function shutdown(signal: NodeJS.Signals): Promise<void> {
  if (closing) {
    return
  }
  closing = true
  app.log.info({ signal }, 'shutting down console API')
  await app.close()
}

process.once('SIGINT', () => void shutdown('SIGINT'))
process.once('SIGTERM', () => void shutdown('SIGTERM'))

try {
  const address = await app.listen({ host: config.host, port: config.port })
  app.log.info({ address }, 'console API listening')
} catch (error) {
  app.log.fatal({ err: error }, 'failed to start console API')
  process.exitCode = 1
}
