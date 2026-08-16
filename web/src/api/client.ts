import type {
  CertificateView,
  CertificateVersionView,
  AccessConfigLimits,
  ConfigurationVersionDetail,
  ConfigurationVersionListResult,
  DraftRevisionView,
  HealthResponse,
  ProjectRoutesModel,
  ProjectRoutesValidationView,
  ProjectReleaseView,
  ProjectView,
  SavedConfigurationVersion,
  SystemStatusResponse,
  TlsSniCertificateSummary,
  TlsSniResolutionView,
  TlsCertificateReleaseView,
} from './types'

export interface ApiClientErrorField {
  path: string
  code: string
  message: string
}

export class ApiClientError extends Error {
  readonly status: number
  readonly code: string
  readonly fields: readonly ApiClientErrorField[]

  constructor(
    message: string,
    status: number,
    code: string,
    fields: readonly ApiClientErrorField[] = [],
  ) {
    super(message)
    this.name = 'ApiClientError'
    this.status = status
    this.code = code
    this.fields = fields
  }
}

function isHealthResponse(value: unknown): value is HealthResponse {
  if (typeof value !== 'object' || value === null) {
    return false
  }

  const candidate = value as Record<string, unknown>
  return (
    candidate.status === 'ok' &&
    candidate.service === 'access-gateway-console-api' &&
    typeof candidate.version === 'string'
  )
}

export async function fetchHealth(signal?: AbortSignal): Promise<HealthResponse> {
  return requestJson('/api/health', { signal }, isHealthResponse)
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function hasPositiveIntegers(value: Record<string, unknown>, keys: readonly string[]): boolean {
  return keys.every(
    (key) =>
      typeof value[key] === 'number' &&
      Number.isSafeInteger(value[key]) &&
      (value[key] as number) > 0,
  )
}

function isAccessConfigLimits(value: unknown): value is AccessConfigLimits {
  if (
    !isRecord(value) ||
    value.schemaVersion !== 1 ||
    !isRecord(value.projectList) ||
    !isRecord(value.projectRoute) ||
    !isRecord(value.grayRules)
  ) {
    return false
  }
  return (
    hasPositiveIntegers(value.projectList, [
      'maxPayloadBytes',
      'maxProjects',
      'maxProjectNameBytes',
    ]) &&
    hasPositiveIntegers(value.projectRoute, [
      'maxPayloadBytes',
      'maxHosts',
      'maxRoutes',
      'maxHostPatternBytes',
      'maxPathBytes',
      'maxMethodBytes',
      'maxServiceBytes',
      'maxClusterBytes',
      'maxConditionBytes',
      'maxScriptBytes',
      'maxTemplateBytes',
      'maxHeaderEntries',
      'maxHeaderNameBytes',
      'maxHeaderValueBytes',
      'maxCidrsPerRoute',
      'maxCidrBytes',
      'maxAddressesPerRoute',
      'maxAddressBytes',
      'maxStaticResponseBodyBytes',
      'maxStaticResponseBytes',
      'maxPathVariables',
      'maxTemplateExpressions',
      'maxCompiledPrograms',
      'maxEstimatedSnapshotBytes',
    ]) &&
    hasPositiveIntegers(value.grayRules, [
      'maxPayloadBytes',
      'maxRules',
      'maxEntryBytes',
      'maxCidrsPerRule',
      'maxCidrBytes',
    ])
  )
}

function isSystemStatus(value: unknown): value is SystemStatusResponse {
  if (!isRecord(value) || !isRecord(value.dependencies)) return false
  const nativeValidator = value.dependencies.nativeValidator
  return (
    (value.status === 'ready' || value.status === 'degraded') &&
    value.service === 'access-gateway-console-api' &&
    isRecord(value.dependencies.database) &&
    typeof value.dependencies.database.status === 'string' &&
    isRecord(nativeValidator) &&
    typeof nativeValidator.contractVersion === 'number' &&
    (nativeValidator.revision === null || typeof nativeValidator.revision === 'string') &&
    (nativeValidator.limits === null || isAccessConfigLimits(nativeValidator.limits))
  )
}

function isProject(value: unknown): value is ProjectView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.domain === 'string' &&
    typeof value.status === 'string' &&
    typeof value.lockVersion === 'string' &&
    typeof value.createdAt === 'string' &&
    typeof value.updatedAt === 'string'
  )
}

function isProjectRoutesModel(value: unknown): value is ProjectRoutesModel {
  return (
    isRecord(value) &&
    value.schemaVersion === 5 &&
    value.kind === 'project_routes_yaml' &&
    isRecord(value.networkPolicy) &&
    (value.networkPolicy.source === 'route' || value.networkPolicy.source === 'project') &&
    (value.networkPolicy.httpsRedirect === 'off' ||
      value.networkPolicy.httpsRedirect === '301' ||
      value.networkPolicy.httpsRedirect === '302' ||
      value.networkPolicy.httpsRedirect === '307' ||
      value.networkPolicy.httpsRedirect === '308') &&
    Array.isArray(value.networkPolicy.allowedCidrs) &&
    value.networkPolicy.allowedCidrs.every((cidr) => typeof cidr === 'string') &&
    Array.isArray(value.networkPolicy.deniedCidrs) &&
    value.networkPolicy.deniedCidrs.every((cidr) => typeof cidr === 'string') &&
    Array.isArray(value.routes) &&
    value.routes.every(
      (route) =>
        isRecord(route) &&
        typeof route.id === 'string' &&
        typeof route.source === 'string' &&
        (route.format === 'yaml' ||
          (route.format === 'js' &&
            typeof route.path === 'string' &&
            (route.method === undefined || typeof route.method === 'string'))),
    )
  )
}

function isDraftRevision(value: unknown): value is DraftRevisionView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.draftId === 'string' &&
    typeof value.revision === 'number' &&
    isProjectRoutesModel(value.model) &&
    typeof value.modelSha256 === 'string' &&
    typeof value.validationState === 'string'
  )
}

function apiErrorMessage(value: unknown, status: number): string {
  if (isRecord(value) && isRecord(value.error) && typeof value.error.message === 'string') {
    return value.error.message
  }
  return `Console API request failed with status ${status}`
}

function apiErrorCode(value: unknown): string {
  return isRecord(value) && isRecord(value.error) && typeof value.error.code === 'string'
    ? value.error.code
    : 'API_ERROR'
}

function apiErrorFields(value: unknown): readonly ApiClientErrorField[] {
  if (!isRecord(value) || !isRecord(value.error) || !Array.isArray(value.error.fields)) return []
  return value.error.fields.filter(
    (field): field is ApiClientErrorField =>
      isRecord(field) &&
      typeof field.path === 'string' &&
      typeof field.code === 'string' &&
      typeof field.message === 'string',
  )
}

async function requestJson<T>(
  path: string,
  init: RequestInit,
  validate: (value: unknown) => value is T,
): Promise<T> {
  const headers = new Headers(init.headers)
  headers.set('Accept', 'application/json')
  const response = await fetch(path, {
    ...init,
    headers,
  })
  const body: unknown = await response.json().catch(() => null)
  if (!response.ok) {
    throw new ApiClientError(
      apiErrorMessage(body, response.status),
      response.status,
      apiErrorCode(body),
      apiErrorFields(body),
    )
  }
  if (!validate(body)) {
    throw new Error('Console API returned an invalid response')
  }
  return body
}

export async function fetchSystemStatus(signal?: AbortSignal): Promise<SystemStatusResponse> {
  return requestJson('/api/system/status', { signal }, isSystemStatus)
}

export async function fetchProjects(signal?: AbortSignal): Promise<readonly ProjectView[]> {
  const response = await requestJson(
    '/api/projects',
    { signal },
    (value): value is { items: ProjectView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isProject),
  )
  return response.items
}

export async function fetchProject(projectId: string, signal?: AbortSignal): Promise<ProjectView> {
  return requestJson(`/api/projects/${encodeURIComponent(projectId)}`, { signal }, isProject)
}

export async function createProject(domain: string): Promise<{ id: string }> {
  return requestJson(
    '/api/projects',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ domain }),
    },
    (value): value is { id: string } => isRecord(value) && typeof value.id === 'string',
  )
}

export async function fetchCurrentDraftRevision(
  draftId: string,
  signal?: AbortSignal,
): Promise<DraftRevisionView | null> {
  const response = await fetch(`/api/drafts/${encodeURIComponent(draftId)}/current-revision`, {
    signal,
    headers: { Accept: 'application/json' },
  })
  if (response.status === 404) return null
  const body: unknown = await response.json().catch(() => null)
  if (!response.ok) throw new Error(apiErrorMessage(body, response.status))
  if (!isDraftRevision(body)) throw new Error('Console API returned an invalid draft revision')
  return body
}

export async function saveDraftRevision(
  draftId: string,
  lockVersion: string,
  model: ProjectRoutesModel,
): Promise<DraftRevisionView> {
  return requestJson(
    `/api/drafts/${encodeURIComponent(draftId)}/revisions`,
    {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'If-Match': `"${lockVersion}"`,
      },
      body: JSON.stringify({ changeSummary: 'Update routes from Console', model }),
    },
    isDraftRevision,
  )
}

function isProjectRoutesValidation(value: unknown): value is ProjectRoutesValidationView {
  return (
    isRecord(value) &&
    typeof value.valid === 'boolean' &&
    Array.isArray(value.issues) &&
    value.issues.every(
      (issue) =>
        isRecord(issue) &&
        typeof issue.routeId === 'string' &&
        typeof issue.path === 'string' &&
        typeof issue.line === 'number' &&
        typeof issue.column === 'number' &&
        typeof issue.code === 'string' &&
        typeof issue.message === 'string',
    ) &&
    (value.wirePreview === null || typeof value.wirePreview === 'string') &&
    (value.wireSha256 === null || typeof value.wireSha256 === 'string') &&
    (value.validator === null ||
      (isRecord(value.validator) &&
        typeof value.validator.contractVersion === 'number' &&
        typeof value.validator.revision === 'string'))
  )
}

export async function validateProjectRoutes(
  projectId: string,
  model: ProjectRoutesModel,
): Promise<ProjectRoutesValidationView> {
  return requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/routes/validate`,
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ model }),
    },
    isProjectRoutesValidation,
  )
}

function isConfigurationVersionSummary(value: unknown): boolean {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.projectId === 'string' &&
    typeof value.number === 'number' &&
    typeof value.changeSummary === 'string' &&
    typeof value.routeCount === 'number' &&
    typeof value.modelSha256 === 'string' &&
    typeof value.validationState === 'string' &&
    typeof value.publicationStatus === 'string' &&
    typeof value.createdAt === 'string'
  )
}

function isConfigurationVersionDetail(value: unknown): value is ConfigurationVersionDetail {
  return (
    isConfigurationVersionSummary(value) && isRecord(value) && isProjectRoutesModel(value.model)
  )
}

function isSavedConfigurationVersion(value: unknown): value is SavedConfigurationVersion {
  return (
    isRecord(value) &&
    isConfigurationVersionDetail(value.version) &&
    typeof value.lockVersion === 'string'
  )
}

function isProjectRelease(value: unknown): value is ProjectReleaseView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.sequence === 'string' &&
    typeof value.projectId === 'string' &&
    (value.kind === 'project_route' || value.kind === 'project_decommission') &&
    typeof value.title === 'string' &&
    typeof value.status === 'string' &&
    (value.sourceConfigurationVersion === null ||
      (isRecord(value.sourceConfigurationVersion) &&
        typeof value.sourceConfigurationVersion.id === 'string' &&
        typeof value.sourceConfigurationVersion.number === 'number')) &&
    Array.isArray(value.resources) &&
    isRecord(value.publication) &&
    value.activationStatus === 'unknown'
  )
}

function isCertificateVersion(value: unknown): value is CertificateVersionView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.version === 'number' &&
    typeof value.status === 'string' &&
    typeof value.subject === 'string' &&
    typeof value.issuer === 'string' &&
    typeof value.serialNumber === 'string' &&
    typeof value.fingerprintSha256 === 'string' &&
    Array.isArray(value.dnsNames) &&
    value.dnsNames.every((name) => typeof name === 'string') &&
    typeof value.notBefore === 'string' &&
    typeof value.notAfter === 'string' &&
    typeof value.keyType === 'string' &&
    typeof value.createdAt === 'string'
  )
}

function isCertificate(value: unknown): value is CertificateView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.name === 'string' &&
    typeof value.lockVersion === 'string' &&
    isCertificateVersion(value.currentVersion) &&
    typeof value.versionCount === 'number' &&
    value.runtimeDeploymentStatus === 'activation_unknown' &&
    typeof value.createdAt === 'string' &&
    typeof value.updatedAt === 'string'
  )
}

function isTlsSniCertificateSummary(value: unknown): value is TlsSniCertificateSummary {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.name === 'string' &&
    typeof value.version === 'number' &&
    typeof value.status === 'string' &&
    typeof value.notAfter === 'string' &&
    typeof value.fingerprintSha256 === 'string' &&
    value.runtimeDeploymentStatus === 'activation_unknown'
  )
}

function isTlsSniResolution(value: unknown): value is TlsSniResolutionView {
  return (
    isRecord(value) &&
    typeof value.serverName === 'string' &&
    (value.resolutionStatus === 'matched' ||
      value.resolutionStatus === 'uncovered' ||
      value.resolutionStatus === 'conflict') &&
    (value.matchKind === null || value.matchKind === 'exact' || value.matchKind === 'wildcard') &&
    (value.certificate === null || isTlsSniCertificateSummary(value.certificate)) &&
    Array.isArray(value.matches) &&
    value.matches.every(isTlsSniCertificateSummary) &&
    value.runtimeDeploymentStatus === 'activation_unknown'
  )
}

function isTlsCertificateRelease(value: unknown): value is TlsCertificateReleaseView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.sequence === 'string' &&
    typeof value.status === 'string' &&
    typeof value.defaultCertificateId === 'string' &&
    typeof value.certificateCount === 'number' &&
    typeof value.wireSha256 === 'string' &&
    isRecord(value.resource) &&
    typeof value.resource.id === 'string' &&
    typeof value.resource.status === 'string' &&
    isRecord(value.publication) &&
    value.activationStatus === 'unknown' &&
    typeof value.createdAt === 'string'
  )
}

export async function fetchCertificates(signal?: AbortSignal): Promise<readonly CertificateView[]> {
  const result = await requestJson(
    '/api/certificates',
    { signal },
    (value): value is { items: CertificateView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isCertificate),
  )
  return result.items
}

export async function createCertificate(input: {
  name: string
  certificatePem: string
  privateKeyPem: string
}): Promise<CertificateView> {
  return requestJson(
    '/api/certificates',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(input),
    },
    isCertificate,
  )
}

export async function createCertificateVersion(
  certificateId: string,
  input: {
    certificatePem: string
    privateKeyPem: string
    lockVersion: string
    confirmSniCoverageChange?: boolean
  },
): Promise<CertificateView> {
  return requestJson(
    `/api/certificates/${encodeURIComponent(certificateId)}/versions`,
    {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'If-Match': `"${input.lockVersion}"`,
      },
      body: JSON.stringify({
        certificatePem: input.certificatePem,
        privateKeyPem: input.privateKeyPem,
        ...(input.confirmSniCoverageChange ? { confirmSniCoverageChange: true } : {}),
      }),
    },
    isCertificate,
  )
}

export async function fetchCertificateVersions(
  certificateId: string,
  signal?: AbortSignal,
): Promise<readonly CertificateVersionView[]> {
  const result = await requestJson(
    `/api/certificates/${encodeURIComponent(certificateId)}/versions`,
    { signal },
    (value): value is { items: CertificateVersionView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isCertificateVersion),
  )
  return result.items
}

export async function resolveTlsSni(
  serverName: string,
  signal?: AbortSignal,
): Promise<TlsSniResolutionView> {
  return requestJson(
    `/api/tls/sni-resolution?serverName=${encodeURIComponent(serverName)}`,
    { signal },
    isTlsSniResolution,
  )
}

export async function fetchTlsCertificateReleases(
  signal?: AbortSignal,
): Promise<readonly TlsCertificateReleaseView[]> {
  const result = await requestJson(
    '/api/tls/releases',
    { signal },
    (value): value is { items: TlsCertificateReleaseView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isTlsCertificateRelease),
  )
  return result.items
}

export async function createTlsCertificateRelease(
  defaultCertificateId: string,
): Promise<TlsCertificateReleaseView> {
  return requestJson(
    '/api/tls/releases',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Idempotency-Key': crypto.randomUUID() },
      body: JSON.stringify({ defaultCertificateId }),
    },
    isTlsCertificateRelease,
  )
}

export async function publishTlsCertificateRelease(
  releaseId: string,
): Promise<TlsCertificateReleaseView> {
  const result = await requestJson(
    `/api/tls/releases/${encodeURIComponent(releaseId)}/publications`,
    { method: 'POST', headers: { 'Idempotency-Key': crypto.randomUUID() } },
    (value): value is { release: TlsCertificateReleaseView } =>
      isRecord(value) && isTlsCertificateRelease(value.release),
  )
  return result.release
}

export async function fetchConfigurationVersions(
  projectId: string,
  signal?: AbortSignal,
): Promise<ConfigurationVersionListResult> {
  return requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/configuration-versions`,
    { signal },
    (value): value is ConfigurationVersionListResult =>
      isRecord(value) &&
      Array.isArray(value.items) &&
      value.items.every(isConfigurationVersionSummary) &&
      (value.currentVersionId === null || typeof value.currentVersionId === 'string') &&
      typeof value.lockVersion === 'string',
  )
}

export async function fetchCurrentConfigurationVersion(
  projectId: string,
  signal?: AbortSignal,
): Promise<SavedConfigurationVersion | null> {
  try {
    return await requestJson(
      `/api/projects/${encodeURIComponent(projectId)}/configuration-versions/current`,
      { signal },
      isSavedConfigurationVersion,
    )
  } catch (error) {
    if (error instanceof ApiClientError && error.status === 404) return null
    throw error
  }
}

export async function fetchConfigurationVersion(
  projectId: string,
  versionId: string,
  signal?: AbortSignal,
): Promise<ConfigurationVersionDetail> {
  return requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/configuration-versions/${encodeURIComponent(versionId)}`,
    { signal },
    isConfigurationVersionDetail,
  )
}

export async function saveConfigurationVersion(
  projectId: string,
  lockVersion: string,
  baseVersionId: string | null,
  changeSummary: string,
  model: ProjectRoutesModel,
  forceSameContent = false,
): Promise<SavedConfigurationVersion> {
  return requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/configuration-versions`,
    {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'If-Match': `"${lockVersion}"`,
        'Idempotency-Key': crypto.randomUUID(),
      },
      body: JSON.stringify({ baseVersionId, changeSummary, forceSameContent, model }),
    },
    isSavedConfigurationVersion,
  )
}

export async function restoreConfigurationVersion(
  projectId: string,
  versionId: string,
  currentVersionId: string,
  lockVersion: string,
  changeSummary: string,
  model?: ProjectRoutesModel,
): Promise<SavedConfigurationVersion> {
  return requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/configuration-versions/${encodeURIComponent(versionId)}/restorations`,
    {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'If-Match': `"${lockVersion}"`,
        'Idempotency-Key': crypto.randomUUID(),
      },
      body: JSON.stringify({
        baseVersionId: currentVersionId,
        changeSummary,
        forceSameContent: false,
        ...(model ? { model } : {}),
      }),
    },
    isSavedConfigurationVersion,
  )
}

export async function fetchProjectReleases(
  projectId: string,
  signal?: AbortSignal,
): Promise<readonly ProjectReleaseView[]> {
  const result = await requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/releases`,
    { signal },
    (value): value is { items: ProjectReleaseView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isProjectRelease),
  )
  return result.items
}

export async function fetchRelease(releaseId: string): Promise<ProjectReleaseView> {
  return requestJson(`/api/releases/${encodeURIComponent(releaseId)}`, {}, isProjectRelease)
}

export async function createRelease(
  projectId: string,
  sourceVersionId: string,
  expectedCurrentVersionId: string,
  title: string,
  description: string,
): Promise<ProjectReleaseView> {
  return requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/releases`,
    {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Idempotency-Key': crypto.randomUUID(),
      },
      body: JSON.stringify({ sourceVersionId, expectedCurrentVersionId, title, description }),
    },
    isProjectRelease,
  )
}

export async function createProjectDecommissionRelease(
  projectId: string,
  lockVersion: string,
  confirmationDomain: string,
  reason: string,
): Promise<ProjectReleaseView> {
  return requestJson(
    `/api/projects/${encodeURIComponent(projectId)}/decommission-releases`,
    {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'If-Match': `"${lockVersion}"`,
        'Idempotency-Key': crypto.randomUUID(),
      },
      body: JSON.stringify({ confirmationDomain, reason }),
    },
    isProjectRelease,
  )
}

export async function queueReleasePublication(releaseId: string): Promise<ProjectReleaseView> {
  const result = await requestJson(
    `/api/releases/${encodeURIComponent(releaseId)}/publications`,
    { method: 'POST', headers: { 'Idempotency-Key': crypto.randomUUID() } },
    (value): value is { jobId: string; state: string; release: ProjectReleaseView } =>
      isRecord(value) &&
      typeof value.jobId === 'string' &&
      typeof value.state === 'string' &&
      isProjectRelease(value.release),
  )
  return result.release
}
