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
  status: 'active' | 'decommissioning' | 'archived'
  lockVersion: string
  draft: {
    id: string
    state: string
    revision: number
    lockVersion: string
  } | null
  publishedVersion: number | null
  activationStatus: 'unknown'
  createdAt: string
  updatedAt: string
}

export interface RouteItemModel {
  id: string
  source: string
}

export type HttpsRedirect = 'off' | '301' | '302' | '307' | '308'

export interface ProjectNetworkPolicy {
  source: 'route' | 'project'
  httpsRedirect: HttpsRedirect
  allowedCidrs: readonly string[]
  deniedCidrs: readonly string[]
}

export interface ProjectRoutesModel {
  schemaVersion: 4
  kind: 'project_routes_yaml'
  networkPolicy: ProjectNetworkPolicy
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
  kind: 'project_route' | 'project_decommission'
  title: string
  description: string
  status: ReleaseStatus
  sourceConfigurationVersion: {
    id: string
    number: number
    relationAtCreation: 'current' | 'historical' | 'unknown'
  } | null
  currentConfigurationVersionAtCreation: { id: string; number: number } | null
  allocatedWireVersion: number | null
  resources: readonly {
    id: string
    kind: 'project_route' | 'project_list'
    dataId: string
    group: string
    operation: 'upsert' | 'remove'
    status: string
  }[]
  publication: { jobId: string | null; state: string | null }
  activationStatus: 'unknown'
  createdAt: string
  publishedAt: string | null
}

export type CertificateFactStatus = 'valid' | 'expiring' | 'expired' | 'superseded'

export interface CertificateVersionView {
  id: string
  version: number
  status: CertificateFactStatus
  subject: string
  issuer: string
  serialNumber: string
  fingerprintSha256: string
  dnsNames: readonly string[]
  notBefore: string
  notAfter: string
  keyType: string
  createdAt: string
}

export interface CertificateView {
  id: string
  name: string
  lockVersion: string
  currentVersion: CertificateVersionView
  versionCount: number
  runtimeDeploymentStatus: 'activation_unknown'
  createdAt: string
  updatedAt: string
}

export interface TlsSniCertificateSummary {
  id: string
  name: string
  version: number
  status: CertificateFactStatus
  notAfter: string
  fingerprintSha256: string
  runtimeDeploymentStatus: 'activation_unknown'
}

export interface TlsSniResolutionView {
  serverName: string
  resolutionStatus: 'matched' | 'uncovered' | 'conflict'
  matchKind: 'exact' | 'wildcard' | null
  certificate: TlsSniCertificateSummary | null
  matches: readonly TlsSniCertificateSummary[]
  runtimeDeploymentStatus: 'activation_unknown'
}

export interface TlsCertificateReleaseView {
  id: string
  sequence: string
  status: ReleaseStatus
  defaultCertificateId: string
  certificateCount: number
  wireSha256: string
  resource: {
    id: string
    dataId: string
    group: string
    status: string
    verifiedSha256: string | null
    verifiedAt: string | null
  }
  publication: { jobId: string | null; state: string | null }
  activationStatus: 'unknown'
  createdAt: string
  publishedAt: string | null
}
