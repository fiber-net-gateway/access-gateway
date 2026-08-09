import type { Actor, AuthService } from './model.js'

export class FixedActorAuthService implements AuthService {
  readonly mode = 'development' as const
  readonly #actor: Actor

  constructor(actor: Actor) {
    this.#actor = actor
  }

  async authenticate(): Promise<Actor> {
    return this.#actor
  }
}

export class UnavailableAuthService implements AuthService {
  readonly mode = 'unavailable' as const

  async authenticate(): Promise<null> {
    return null
  }
}
