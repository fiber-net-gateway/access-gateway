import { isIP } from 'node:net'
import { domainToASCII } from 'node:url'

import { badRequest, notFound, unavailable } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { TlsSniResolutionView } from './model.js'
import { TlsSniRepository } from './repository.js'

export function normalizeSniServerName(value: string): string {
  const input = value.trim().replace(/\.$/u, '')
  const hostname = domainToASCII(input).toLowerCase()
  const labels = hostname.split('.')
  const valid =
    hostname.length > 0 &&
    hostname.length <= 253 &&
    isIP(hostname) === 0 &&
    !input.startsWith('*.') &&
    labels.every(
      (label) =>
        label.length >= 1 && label.length <= 63 && /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/u.test(label),
    )
  if (!valid) {
    throw badRequest('INVALID_SNI_SERVER_NAME', 'SNI server name must be an exact DNS hostname', [
      {
        path: 'serverName',
        code: 'INVALID_FORMAT',
        message: 'Invalid SNI DNS name',
      },
    ])
  }
  return hostname
}

export interface TlsSniService {
  resolve(actor: Actor, serverName: string): Promise<TlsSniResolutionView>
}

export class DefaultTlsSniService implements TlsSniService {
  readonly #selectors: TlsSniRepository
  readonly #environments: EnvironmentRepository

  constructor(selectors: TlsSniRepository, environments: EnvironmentRepository) {
    this.#selectors = selectors
    this.#environments = environments
  }

  async resolve(actor: Actor, serverName: string): Promise<TlsSniResolutionView> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) throw notFound('Deployment workspace')
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    return this.#selectors.resolve(environmentInternalId, normalizeSniServerName(serverName))
  }
}

export class UnavailableTlsSniService implements TlsSniService {
  async resolve(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
