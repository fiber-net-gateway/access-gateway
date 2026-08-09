import { unauthorized } from '../../shared/errors.js'
import type { Actor, AuthService } from './model.js'

export async function requireActor(auth: AuthService): Promise<Actor> {
  const actor = await auth.authenticate()
  if (!actor) {
    throw unauthorized()
  }
  return actor
}
