import type { DatabasePool } from '../../database/types.js'
import { checkDatabase } from '../../database/pool.js'
import { currentSchemaVersion, expectedSchemaVersion } from '../../database/schema.js'
import type { AuthService } from '../auth/model.js'
import type { NativeValidator } from '../../integrations/native-validator/model.js'

export type CapabilityStatus = 'ready' | 'unconfigured' | 'unavailable'

export interface CapabilityView {
  status: CapabilityStatus
  detail: string
}

export interface SystemStatusView {
  status: 'ready' | 'degraded'
  service: 'access-gateway-console-api'
  dependencies: {
    database: CapabilityView
    schema: CapabilityView
    authentication: CapabilityView
    nativeValidator: CapabilityView & { contractVersion: number; revision: string | null }
    publicationWorker: CapabilityView
    activationCollector: CapabilityView
  }
}

export interface SystemStatusService {
  get(): Promise<SystemStatusView>
}

export class DefaultSystemStatusService implements SystemStatusService {
  readonly #pool: DatabasePool | null
  readonly #auth: AuthService
  readonly #validator: NativeValidator
  readonly #validatorDetail: string
  readonly #validatorConfigured: boolean

  constructor(
    pool: DatabasePool | null,
    auth: AuthService,
    validator: NativeValidator,
    validatorDetail = validator.available ? 'configured' : 'not configured',
    validatorConfigured = validator.available,
  ) {
    this.#pool = pool
    this.#auth = auth
    this.#validator = validator
    this.#validatorDetail = validatorDetail
    this.#validatorConfigured = validatorConfigured
  }

  async get(): Promise<SystemStatusView> {
    let database: CapabilityView
    let schema: CapabilityView
    if (!this.#pool) {
      database = { status: 'unconfigured', detail: 'MySQL is not configured' }
      schema = { status: 'unconfigured', detail: `expected ${expectedSchemaVersion}` }
    } else {
      try {
        await checkDatabase(this.#pool)
        database = { status: 'ready', detail: 'MySQL connection is healthy' }
      } catch {
        database = { status: 'unavailable', detail: 'MySQL connection check failed' }
      }
      try {
        const version = await currentSchemaVersion(this.#pool)
        schema =
          version === expectedSchemaVersion
            ? { status: 'ready', detail: version }
            : {
                status: 'unavailable',
                detail: `expected ${expectedSchemaVersion}, found ${version ?? 'none'}`,
              }
      } catch {
        schema = { status: 'unavailable', detail: `expected ${expectedSchemaVersion}` }
      }
    }

    const dependencies: SystemStatusView['dependencies'] = {
      database,
      schema,
      authentication:
        this.#auth.mode === 'unavailable'
          ? { status: 'unavailable', detail: 'authentication provider is unavailable' }
          : { status: 'ready', detail: this.#auth.mode },
      nativeValidator: {
        status: this.#validator.available
          ? 'ready'
          : this.#validatorConfigured
            ? 'unavailable'
            : 'unconfigured',
        detail: this.#validatorDetail,
        contractVersion: this.#validator.contractVersion,
        revision: this.#validator.revision,
      },
      publicationWorker: {
        status: 'unconfigured',
        detail: 'publication worker and Nacos adapter are not configured',
      },
      activationCollector: {
        status: 'unconfigured',
        detail: 'access-server status endpoints are not configured',
      },
    }
    const ready = database.status === 'ready' && schema.status === 'ready'
    return {
      status: ready ? 'ready' : 'degraded',
      service: 'access-gateway-console-api',
      dependencies,
    }
  }
}
