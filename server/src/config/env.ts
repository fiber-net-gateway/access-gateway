import { isAbsolute } from 'node:path'

const logLevels = ['fatal', 'error', 'warn', 'info', 'debug', 'trace', 'silent'] as const

export type LogLevel = (typeof logLevels)[number]

export type AppEnvironment = 'development' | 'test' | 'production'
export type AuthMode = 'development' | 'oidc'

export interface DatabaseConfig {
  enabled: boolean
  host: string
  port: number
  database: string
  user: string
  password: string
  connectionLimit: number
  sslMode: 'disabled' | 'required'
}

export interface DocumentEncryptionConfig {
  keyId: string
  key: Buffer | null
}

export interface AuthConfig {
  mode: AuthMode
  developmentSubject: string
  developmentDisplayName: string
}

export interface NativeValidatorConfig {
  path: string | null
  contractVersion: number
  timeoutMillis: number
  maxInputBytes: number
  maxOutputBytes: number
}

export interface ServerConfig {
  host: string
  port: number
  logLevel: LogLevel
  environment: AppEnvironment
  staticRoot: string | null
  database: DatabaseConfig
  documentEncryption: DocumentEncryptionConfig
  auth: AuthConfig
  nativeValidator: NativeValidatorConfig
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

function parsePort(value: string | undefined, name = 'APP_PORT', defaultValue = 3000): number {
  if (value === undefined) {
    return defaultValue
  }
  if (!/^\d+$/u.test(value)) {
    throw new Error(`${name} must be an integer between 1 and 65535`)
  }
  const port = Number(value)
  if (!Number.isSafeInteger(port) || port < 1 || port > 65535) {
    throw new Error(`${name} must be an integer between 1 and 65535`)
  }
  return port
}

function parsePositiveInteger(
  value: string | undefined,
  name: string,
  defaultValue: number,
): number {
  if (value === undefined) {
    return defaultValue
  }
  if (!/^\d+$/u.test(value)) {
    throw new Error(`${name} must be a positive integer`)
  }
  const parsed = Number(value)
  if (!Number.isSafeInteger(parsed) || parsed < 1) {
    throw new Error(`${name} must be a positive integer`)
  }
  return parsed
}

function parseBoolean(value: string | undefined, name: string, defaultValue: boolean): boolean {
  if (value === undefined) {
    return defaultValue
  }
  if (value === 'true') {
    return true
  }
  if (value === 'false') {
    return false
  }
  throw new Error(`${name} must be true or false`)
}

function parseEnum<T extends string>(
  value: string | undefined,
  name: string,
  values: readonly T[],
  defaultValue: T,
): T {
  if (value === undefined) {
    return defaultValue
  }
  if ((values as readonly string[]).includes(value)) {
    return value as T
  }
  throw new Error(`${name} must be one of: ${values.join(', ')}`)
}

function parseNonEmpty(value: string | undefined, name: string, defaultValue: string): string {
  const parsed = value === undefined ? defaultValue : value.trim()
  if (parsed.length === 0) {
    throw new Error(`${name} must not be empty`)
  }
  return parsed
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

function parseDocumentKey(value: string | undefined): Buffer | null {
  if (!value) return null
  if (!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value)) {
    return Buffer.alloc(0)
  }
  const key = Buffer.from(value, 'base64')
  return key.toString('base64') === value ? key : Buffer.alloc(0)
}

export function loadServerConfig(env: NodeJS.ProcessEnv = process.env): ServerConfig {
  const environment = parseEnum(
    env.NODE_ENV,
    'NODE_ENV',
    ['development', 'test', 'production'] as const,
    'development',
  )
  const databaseEnabled = parseBoolean(env.MYSQL_ENABLED, 'MYSQL_ENABLED', false)
  const databasePassword = env.MYSQL_PASSWORD ?? ''
  if (databaseEnabled && databasePassword.length === 0) {
    throw new Error('MYSQL_PASSWORD must be set when MYSQL_ENABLED=true')
  }

  const authMode = parseEnum(
    env.AUTH_MODE,
    'AUTH_MODE',
    ['development', 'oidc'] as const,
    'development',
  )
  if (environment === 'production' && authMode === 'development') {
    throw new Error('AUTH_MODE=development is not allowed in production')
  }

  const documentKey = parseDocumentKey(env.DOCUMENT_ENCRYPTION_KEY_BASE64)
  if (databaseEnabled && documentKey?.length !== 32) {
    throw new Error(
      'DOCUMENT_ENCRYPTION_KEY_BASE64 must decode to 32 bytes when MYSQL_ENABLED=true',
    )
  }

  const nativeValidatorPath = env.NATIVE_VALIDATOR_PATH?.trim() || null
  if (nativeValidatorPath && !isAbsolute(nativeValidatorPath)) {
    throw new Error('NATIVE_VALIDATOR_PATH must be an absolute path')
  }
  const staticRoot = env.CONSOLE_STATIC_ROOT?.trim() || null
  if (staticRoot && !isAbsolute(staticRoot)) {
    throw new Error('CONSOLE_STATIC_ROOT must be an absolute path')
  }

  return {
    host: parseHost(env.APP_HOST),
    port: parsePort(env.APP_PORT, 'APP_PORT', 3000),
    logLevel: parseLogLevel(env.LOG_LEVEL),
    environment,
    staticRoot,
    database: {
      enabled: databaseEnabled,
      host: parseNonEmpty(env.MYSQL_HOST, 'MYSQL_HOST', '127.0.0.1'),
      port: parsePort(env.MYSQL_PORT, 'MYSQL_PORT', 3306),
      database: parseNonEmpty(env.MYSQL_DATABASE, 'MYSQL_DATABASE', 'access_gateway'),
      user: parseNonEmpty(env.MYSQL_USER, 'MYSQL_USER', 'access_gateway'),
      password: databasePassword,
      connectionLimit: parsePositiveInteger(
        env.MYSQL_CONNECTION_LIMIT,
        'MYSQL_CONNECTION_LIMIT',
        10,
      ),
      sslMode: parseEnum(
        env.MYSQL_SSL_MODE,
        'MYSQL_SSL_MODE',
        ['disabled', 'required'] as const,
        'disabled',
      ),
    },
    documentEncryption: {
      keyId: parseNonEmpty(
        env.DOCUMENT_ENCRYPTION_KEY_ID,
        'DOCUMENT_ENCRYPTION_KEY_ID',
        'local-v1',
      ),
      key: documentKey,
    },
    auth: {
      mode: authMode,
      developmentSubject: parseNonEmpty(
        env.AUTH_DEVELOPMENT_SUBJECT,
        'AUTH_DEVELOPMENT_SUBJECT',
        'local-developer',
      ),
      developmentDisplayName: parseNonEmpty(
        env.AUTH_DEVELOPMENT_DISPLAY_NAME,
        'AUTH_DEVELOPMENT_DISPLAY_NAME',
        'Local Developer',
      ),
    },
    nativeValidator: {
      path: nativeValidatorPath,
      contractVersion: parsePositiveInteger(
        env.NATIVE_VALIDATOR_CONTRACT_VERSION,
        'NATIVE_VALIDATOR_CONTRACT_VERSION',
        1,
      ),
      timeoutMillis: parsePositiveInteger(
        env.NATIVE_VALIDATOR_TIMEOUT_MILLIS,
        'NATIVE_VALIDATOR_TIMEOUT_MILLIS',
        10_000,
      ),
      maxInputBytes: parsePositiveInteger(
        env.NATIVE_VALIDATOR_MAX_INPUT_BYTES,
        'NATIVE_VALIDATOR_MAX_INPUT_BYTES',
        4_194_304,
      ),
      maxOutputBytes: parsePositiveInteger(
        env.NATIVE_VALIDATOR_MAX_OUTPUT_BYTES,
        'NATIVE_VALIDATOR_MAX_OUTPUT_BYTES',
        1_048_576,
      ),
    },
  }
}
