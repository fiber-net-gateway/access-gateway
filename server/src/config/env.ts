const logLevels = ['fatal', 'error', 'warn', 'info', 'debug', 'trace', 'silent'] as const

export type LogLevel = (typeof logLevels)[number]

export interface ServerConfig {
  host: string
  port: number
  logLevel: LogLevel
}

function isLogLevel(value: string): value is LogLevel {
  return (logLevels as readonly string[]).includes(value)
}

function parseHost(value: string | undefined): string {
  if (value === undefined) {
    return '127.0.0.1'
  }
  const host = value.trim()
  if (host.length === 0 || host.length > 255 || /[\s/]/u.test(host)) {
    throw new Error('APP_HOST must be a non-empty hostname or IP address without whitespace')
  }
  return host
}

function parsePort(value: string | undefined): number {
  if (value === undefined) {
    return 3000
  }
  if (!/^\d+$/u.test(value)) {
    throw new Error('APP_PORT must be an integer between 1 and 65535')
  }
  const port = Number(value)
  if (!Number.isSafeInteger(port) || port < 1 || port > 65535) {
    throw new Error('APP_PORT must be an integer between 1 and 65535')
  }
  return port
}

function parseLogLevel(value: string | undefined): LogLevel {
  if (value === undefined) {
    return 'info'
  }
  const logLevel = value.trim().toLowerCase()
  if (!isLogLevel(logLevel)) {
    throw new Error(`LOG_LEVEL must be one of: ${logLevels.join(', ')}`)
  }
  return logLevel
}

export function loadServerConfig(env: NodeJS.ProcessEnv = process.env): ServerConfig {
  return {
    host: parseHost(env.APP_HOST),
    port: parsePort(env.APP_PORT),
    logLevel: parseLogLevel(env.LOG_LEVEL),
  }
}
