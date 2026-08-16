import { notFound, unavailable } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import type { ActivationInstanceList, ActivationService } from './model.js'
import { ActivationReadRepository } from './read-repository.js'

export class DefaultActivationService implements ActivationService {
  readonly #repository: ActivationReadRepository

  constructor(repository: ActivationReadRepository) {
    this.#repository = repository
  }

  async listReleaseInstances(
    actor: Actor,
    releaseId: string,
    cursor: string | null,
    limit: number,
  ): Promise<ActivationInstanceList> {
    const result = await this.#repository.listReleaseInstances(actor, releaseId, cursor, limit)
    if (!result) throw notFound('Release')
    return result
  }
}

export class UnavailableActivationService implements ActivationService {
  async listReleaseInstances(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
