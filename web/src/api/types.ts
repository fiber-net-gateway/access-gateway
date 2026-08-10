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

export interface ProjectView {
  id: string
  domain: string
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

export interface RouteItemModel {
  id: string
  source: string
}

export interface ProjectRoutesModel {
  schemaVersion: 2
  kind: 'project_routes_yaml'
  routes: readonly RouteItemModel[]
}

export interface DraftRevisionView {
  id: string
  draftId: string
  revision: number
  model: ProjectRoutesModel
  modelSha256: string
  validationState: 'not_run' | 'pending' | 'valid' | 'invalid'
  changeSummary: string
  createdAt: string
}

export interface RouteValidationIssue {
  routeId: string
  path: string
  line: number
  column: number
  code: string
  message: string
}

export interface ProjectRoutesValidationView {
  valid: boolean
  issues: readonly RouteValidationIssue[]
  wirePreview: string | null
  wireSha256: string | null
  validator: {
    contractVersion: number
    revision: string
  } | null
}

export type ConfigurationVersionValidationState = 'not_run' | 'pending' | 'valid' | 'invalid'

export interface ConfigurationVersionSummary {
  id: string
  projectId: string
  number: number
  relation: 'current' | 'historical'
  baseVersionId: string | null
  restoredFromVersionId: string | null
  changeSummary: string
  routeCount: number
  modelSha256: string
  validationState: ConfigurationVersionValidationState
  publicationStatus: string
  createdBy: { id: string; displayName: string }
  createdAt: string
}

export interface ConfigurationVersionDetail extends ConfigurationVersionSummary {
  model: ProjectRoutesModel
}

export interface ConfigurationVersionListResult {
  items: readonly ConfigurationVersionSummary[]
  nextCursor: string | null
  currentVersionId: string | null
  lockVersion: string
}

export interface SavedConfigurationVersion {
  version: ConfigurationVersionDetail
  lockVersion: string
}

export type ReleaseStatus =
  | 'creating'
  | 'validating'
  | 'validation_failed'
  | 'ready'
  | 'queued'
  | 'publishing'
  | 'published'
  | 'partially_published'
  | 'publish_failed'
  | 'canceled'
  | 'superseded'
  | 'abandoned'

export interface ProjectReleaseView {
  id: string
  sequence: string
  projectId: string
  title: string
  description: string
  status: ReleaseStatus
  sourceConfigurationVersion: {
    id: string
    number: number
    relationAtCreation: 'current' | 'historical' | 'unknown'
  }
  currentConfigurationVersionAtCreation: { id: string; number: number }
  allocatedWireVersion: number
  resources: readonly {
    id: string
    kind: 'project_route' | 'project_list'
    dataId: string
    group: string
    status: string
  }[]
  publication: { jobId: string | null; state: string | null }
  activationStatus: 'unknown'
  createdAt: string
  publishedAt: string | null
}
