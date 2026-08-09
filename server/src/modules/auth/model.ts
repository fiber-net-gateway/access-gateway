export const environmentRoles = ['admin', 'maintainer', 'publisher', 'auditor'] as const

export type EnvironmentRole = (typeof environmentRoles)[number]

export interface Actor {
  internalId: string
  publicId: string
  subject: string
  displayName: string
  platformAdmin: boolean
}

export interface AuthService {
  authenticate(): Promise<Actor | null>
  readonly mode: 'development' | 'oidc' | 'unavailable'
}
