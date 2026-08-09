export interface HealthResponse {
  status: 'ok'
  service: 'access-gateway-console-api'
  version: string
}

export type ApiConnectionState = 'loading' | 'online' | 'offline'

export type CapabilityStatus = 'ready' | 'unconfigured' | 'unavailable'

export interface CapabilityView {
  status: CapabilityStatus
  detail: string
}

export interface SystemStatusResponse {
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

export interface EnvironmentView {
  id: string
  code: string
  name: string
  tier: 'local' | 'test' | 'staging' | 'production'
  status: 'active' | 'disabled'
  nacos: {
    endpoint: string
    namespace: string
    tenant: string
    credentialConfigured: boolean
  }
  lockVersion: string
}

export interface ProjectView {
  id: string
  environmentId: string
  name: string
  status: 'active' | 'archived'
  draft: {
    id: string
    state: string
    revision: number
    lockVersion: string
  } | null
  publishedVersion: number | null
  activationStatus: 'unknown'
}

export interface CreateEnvironmentInput {
  code: string
  name: string
  tier: EnvironmentView['tier']
  nacosEndpoint: string
}
