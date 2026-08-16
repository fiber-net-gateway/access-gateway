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

export interface PublicationConfig {
  enabled: boolean
  endpointOverride: string | null
  requestTimeoutMillis: number
  maxResponseBytes: number
  pollIntervalMillis: number
  leaseMillis: number
}

export interface ActivationTargetConfig {
  environmentCode: string
  instanceKey: string
  endpoint: string
  token: string
}

export interface ActivationCollectorConfig {
  enabled: boolean
  targets: readonly ActivationTargetConfig[]
  requestTimeoutMillis: number
  maxResponseBytes: number
  pollIntervalMillis: number
  evidenceTtlMillis: number
  leaseMillis: number
  maxPages: number
  maxProjects: number
  concurrency: number
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
  publication: PublicationConfig
  activation: ActivationCollectorConfig
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

function parseBoundedPositiveInteger(
  value: string | undefined,
  name: string,
  defaultValue: number,
  minimum: number,
  maximum: number,
): number {
  const parsed = parsePositiveInteger(value, name, defaultValue)
  if (parsed < minimum || parsed > maximum) {
    throw new Error(`${name} must be an integer between ${minimum} and ${maximum}`)
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

function parseOptionalHttpEndpoint(value: string | undefined, name: string): string | null {
  const raw = value?.trim()
  if (!raw) return null
  let endpoint: URL
  try {
    endpoint = new URL(raw)
  } catch {
    throw new Error(`${name} must be an absolute HTTP(S) URL`)
  }
  if (
    !['http:', 'https:'].includes(endpoint.protocol) ||
    endpoint.username ||
    endpoint.password ||
    endpoint.search ||
    endpoint.hash ||
    endpoint.pathname !== '/'
  ) {
    throw new Error(`${name} must be an HTTP(S) origin without credentials, path, or query data`)
  }
  return endpoint.origin
}

function parseActivationTargets(value: string | undefined): readonly ActivationTargetConfig[] {
  const raw = value?.trim()
  if (!raw) return []
  if (Buffer.byteLength(raw, 'utf8') > 131_072) {
    throw new Error('ACTIVATION_TARGETS_JSON exceeds 131072 bytes')
  }

  let parsed: unknown
  try {
    parsed = JSON.parse(raw)
  } catch {
    throw new Error('ACTIVATION_TARGETS_JSON must be valid JSON')
  }
  if (!Array.isArray(parsed) || parsed.length < 1 || parsed.length > 64) {
    throw new Error('ACTIVATION_TARGETS_JSON must contain 1-64 targets')
  }

  const identities = new Set<string>()
  return parsed.map((candidate, index) => {
    if (!candidate || typeof candidate !== 'object' || Array.isArray(candidate)) {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}] must be an object`)
    }
    const record = candidate as Record<string, unknown>
    const keys = Object.keys(record).sort()
    if (
      keys.length !== 4 ||
      !['endpoint', 'environmentCode', 'instanceKey', 'token'].every((key) => keys.includes(key))
    ) {
      throw new Error(
        `ACTIVATION_TARGETS_JSON[${index}] must contain only environmentCode, instanceKey, endpoint, and token`,
      )
    }
    const environmentCode = record.environmentCode
    const instanceKey = record.instanceKey
    const endpointText = record.endpoint
    const token = record.token
    if (typeof environmentCode !== 'string' || !/^[a-z][a-z0-9-]{0,63}$/u.test(environmentCode)) {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}].environmentCode is invalid`)
    }
    if (
      typeof instanceKey !== 'string' ||
      instanceKey.length < 1 ||
      instanceKey.length > 255 ||
      !/^[A-Za-z0-9._:-]+$/u.test(instanceKey)
    ) {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}].instanceKey is invalid`)
    }
    if (
      typeof token !== 'string' ||
      token.length < 32 ||
      token.length > 512 ||
      !/^[\x21-\x7e]+$/u.test(token)
    ) {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}].token is invalid`)
    }
    if (typeof endpointText !== 'string') {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}].endpoint is invalid`)
    }
    if (Buffer.byteLength(endpointText, 'utf8') > 2_048) {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}].endpoint exceeds 2048 bytes`)
    }
    let endpoint: URL
    try {
      endpoint = new URL(endpointText)
    } catch {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}].endpoint is invalid`)
    }
    if (
      !['http:', 'https:'].includes(endpoint.protocol) ||
      endpoint.username ||
      endpoint.password ||
      endpoint.search ||
      endpoint.hash ||
      endpoint.pathname !== '/v1/activation-evidence'
    ) {
      throw new Error(
        `ACTIVATION_TARGETS_JSON[${index}].endpoint must be an HTTP(S) activation-evidence URL without credentials or query data`,
      )
    }
    if (Buffer.byteLength(endpoint.href, 'utf8') > 2_048) {
      throw new Error(`ACTIVATION_TARGETS_JSON[${index}].endpoint exceeds 2048 bytes`)
    }
    const identity = `${environmentCode}\n${instanceKey}`
    if (identities.has(identity)) {
      throw new Error(`ACTIVATION_TARGETS_JSON contains a duplicate target at index ${index}`)
    }
    identities.add(identity)
    return {
      environmentCode,
      instanceKey,
      endpoint: endpoint.href,
      token,
    }
  })
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
  const activationEnabled = parseBoolean(
    env.ACTIVATION_COLLECTOR_ENABLED,
    'ACTIVATION_COLLECTOR_ENABLED',
    false,
  )
  const activationTargets = parseActivationTargets(env.ACTIVATION_TARGETS_JSON)
  if (!activationEnabled && activationTargets.length > 0) {
    throw new Error('ACTIVATION_TARGETS_JSON is only valid when ACTIVATION_COLLECTOR_ENABLED=true')
  }
  const activation: ActivationCollectorConfig = {
    enabled: activationEnabled,
    targets: activationTargets,
    requestTimeoutMillis: parseBoundedPositiveInteger(
      env.ACTIVATION_REQUEST_TIMEOUT_MILLIS,
      'ACTIVATION_REQUEST_TIMEOUT_MILLIS',
      5_000,
      100,
      60_000,
    ),
    maxResponseBytes: parseBoundedPositiveInteger(
      env.ACTIVATION_MAX_RESPONSE_BYTES,
      'ACTIVATION_MAX_RESPONSE_BYTES',
      1_048_576,
      4_096,
      16_777_216,
    ),
    pollIntervalMillis: parseBoundedPositiveInteger(
      env.ACTIVATION_POLL_INTERVAL_MILLIS,
      'ACTIVATION_POLL_INTERVAL_MILLIS',
      5_000,
      100,
      3_600_000,
    ),
    evidenceTtlMillis: parseBoundedPositiveInteger(
      env.ACTIVATION_EVIDENCE_TTL_MILLIS,
      'ACTIVATION_EVIDENCE_TTL_MILLIS',
      15_000,
      1_000,
      86_400_000,
    ),
    leaseMillis: parseBoundedPositiveInteger(
      env.ACTIVATION_LEASE_MILLIS,
      'ACTIVATION_LEASE_MILLIS',
      30_000,
      1_000,
      3_600_000,
    ),
    maxPages: parseBoundedPositiveInteger(
      env.ACTIVATION_MAX_PAGES,
      'ACTIVATION_MAX_PAGES',
      16,
      1,
      256,
    ),
    maxProjects: parseBoundedPositiveInteger(
      env.ACTIVATION_MAX_PROJECTS,
      'ACTIVATION_MAX_PROJECTS',
      1_024,
      1,
      65_536,
    ),
    concurrency: parseBoundedPositiveInteger(
      env.ACTIVATION_CONCURRENCY,
      'ACTIVATION_CONCURRENCY',
      4,
      1,
      64,
    ),
  }
  if (activation.evidenceTtlMillis <= activation.pollIntervalMillis) {
    throw new Error('ACTIVATION_EVIDENCE_TTL_MILLIS must exceed ACTIVATION_POLL_INTERVAL_MILLIS')
  }
  if (activation.leaseMillis < activation.requestTimeoutMillis * 2) {
    throw new Error(
      'ACTIVATION_LEASE_MILLIS must be at least twice ACTIVATION_REQUEST_TIMEOUT_MILLIS',
    )
  }
  if (activation.maxProjects > activation.maxPages * 256) {
    throw new Error(
      'ACTIVATION_MAX_PAGES must cover ACTIVATION_MAX_PROJECTS at 256 Projects per page',
    )
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
    publication: {
      enabled: parseBoolean(env.PUBLICATION_WORKER_ENABLED, 'PUBLICATION_WORKER_ENABLED', false),
      endpointOverride: parseOptionalHttpEndpoint(
        env.PUBLICATION_NACOS_ENDPOINT,
        'PUBLICATION_NACOS_ENDPOINT',
      ),
      requestTimeoutMillis: parsePositiveInteger(
        env.NACOS_REQUEST_TIMEOUT_MILLIS,
        'NACOS_REQUEST_TIMEOUT_MILLIS',
        5_000,
      ),
      maxResponseBytes: parsePositiveInteger(
        env.NACOS_MAX_RESPONSE_BYTES,
        'NACOS_MAX_RESPONSE_BYTES',
        5_242_880,
      ),
      pollIntervalMillis: parsePositiveInteger(
        env.PUBLICATION_POLL_INTERVAL_MILLIS,
        'PUBLICATION_POLL_INTERVAL_MILLIS',
        1_000,
      ),
      leaseMillis: parsePositiveInteger(
        env.PUBLICATION_LEASE_MILLIS,
        'PUBLICATION_LEASE_MILLIS',
        30_000,
      ),
    },
    activation,
  }
}
