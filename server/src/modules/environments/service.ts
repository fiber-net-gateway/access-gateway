import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import {
  environmentTiers,
  type CreateEnvironmentInput,
  type EnvironmentListResult,
  type EnvironmentView,
} from './model.js'
import { EnvironmentRepository } from './repository.js'

export interface EnvironmentService {
  list(actor: Actor): Promise<EnvironmentListResult>
  get(actor: Actor, id: string): Promise<EnvironmentView>
  create(actor: Actor, input: CreateEnvironmentInput, requestId: string): Promise<EnvironmentView>
}

function validateCode(value: string): string {
  const code = value.trim()
  if (!/^[a-z][a-z0-9-]{1,62}[a-z0-9]$/u.test(code)) {
    throw badRequest(
      'INVALID_ENVIRONMENT_CODE',
      'Environment code must be 3-64 lowercase letters, numbers, or hyphens',
      [{ path: 'code', code: 'INVALID_FORMAT', message: 'Invalid environment code' }],
    )
  }
  return code
}

function validateEndpoint(value: string): string {
  let url: URL
  try {
    url = new URL(value)
  } catch {
    throw badRequest('INVALID_NACOS_ENDPOINT', 'Nacos endpoint must be an HTTP or HTTPS URL')
  }
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password) {
    throw badRequest(
      'INVALID_NACOS_ENDPOINT',
      'Nacos endpoint must use HTTP or HTTPS and must not contain credentials',
    )
  }
  return url.toString().replace(/\/$/u, '')
}

function validateDisplayName(value: string): string {
  const name = value.trim()
  if (name.length === 0 || name.length > 255) {
    throw badRequest('INVALID_ENVIRONMENT_NAME', 'Environment name must be 1-255 characters', [
      { path: 'name', code: 'INVALID_LENGTH', message: 'Invalid environment name' },
    ])
  }
  return name
}

function validateAsciiContractValue(value: string, path: string, maxLength: number): string {
  if (
    typeof value !== 'string' ||
    value.length === 0 ||
    value.length > maxLength ||
    !/^[\x21-\x7e]+$/u.test(value)
  ) {
    throw badRequest('INVALID_NACOS_CONTRACT', `${path} must be printable ASCII`, [
      { path, code: 'INVALID_FORMAT', message: 'Invalid Nacos contract value' },
    ])
  }
  return value
}

function validateDataIds(input: CreateEnvironmentInput): CreateEnvironmentInput['dataIds'] {
  if (!input.dataIds) return undefined
  return Object.fromEntries(
    Object.entries(input.dataIds).map(([key, value]) => [
      key,
      validateAsciiContractValue(value, `dataIds.${key}`, key.includes('Group') ? 255 : 512),
    ]),
  )
}

export class DefaultEnvironmentService implements EnvironmentService {
  readonly #repository: EnvironmentRepository

  constructor(repository: EnvironmentRepository) {
    this.#repository = repository
  }

  async list(actor: Actor): Promise<EnvironmentListResult> {
    return { items: await this.#repository.list(actor) }
  }

  async get(actor: Actor, id: string): Promise<EnvironmentView> {
    const environment = await this.#repository.findAccessibleByPublicId(actor, id)
    if (!environment) {
      throw notFound('Environment')
    }
    return environment
  }

  async create(
    actor: Actor,
    input: CreateEnvironmentInput,
    requestId: string,
  ): Promise<EnvironmentView> {
    if (!actor.platformAdmin) {
      throw forbidden()
    }
    if (!environmentTiers.includes(input.tier)) {
      throw badRequest('INVALID_ENVIRONMENT_TIER', 'Environment tier is invalid')
    }
    return this.#repository.create(
      actor,
      {
        ...input,
        code: validateCode(input.code),
        name: validateDisplayName(input.name),
        nacosEndpoint: validateEndpoint(input.nacosEndpoint),
        nacosNamespace: input.nacosNamespace?.trim() || undefined,
        nacosTenant: input.nacosTenant?.trim() ?? undefined,
        zone: input.zone?.trim() ?? undefined,
        dataIds: validateDataIds(input),
      },
      requestId,
    )
  }
}

export class UnavailableEnvironmentService implements EnvironmentService {
  async list(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async get(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async create(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
