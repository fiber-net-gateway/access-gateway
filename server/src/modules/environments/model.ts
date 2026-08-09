export const environmentTiers = ['local', 'test', 'staging', 'production'] as const
export type EnvironmentTier = (typeof environmentTiers)[number]

export interface EnvironmentDataIds {
  projects: string
  routePrefix: string
  routeGroup: string
  gray: string
  grayGroup: string
  namingGroup: string
}

export interface EnvironmentView {
  id: string
  code: string
  name: string
  tier: EnvironmentTier
  status: 'active' | 'disabled'
  nacos: {
    endpoint: string
    namespace: string
    tenant: string
    credentialConfigured: boolean
  }
  dataIds: EnvironmentDataIds
  zone: string
  protectionPolicy: Readonly<Record<string, unknown>>
  lockVersion: string
  createdAt: string
  updatedAt: string
}

export interface CreateEnvironmentInput {
  code: string
  name: string
  tier: EnvironmentTier
  nacosEndpoint: string
  nacosNamespace?: string
  nacosTenant?: string
  zone?: string
  dataIds?: Partial<EnvironmentDataIds>
  protectionPolicy?: Readonly<Record<string, unknown>>
}

export interface EnvironmentListResult {
  items: readonly EnvironmentView[]
}
