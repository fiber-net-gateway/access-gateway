import type {
  ActivationInstanceList,
  ActivationInstanceView,
  ActivationStatus,
  ActivationSummary,
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
  WafCompilePreviewView,
  WafEdgeFunctionDraftFunction,
  WafEdgeFunctionMountDraft,
  WafEdgeFunctionsDraft,
  WafEdgeFunctionsDraftView,
  WafEdgeFunctionsView,
  WafFunctionCompilation,
  WafGroup,
  WafHitsView,
  WafHitsWindow,
  WafRulePackView,
  WafMode,
  WafPolicy,
  WafPolicyDraftView,
  WafPolicyIssue,
  WafEdgeFunctionCompilePreviewView,
  WafReleaseResourceView,
  WafReleaseView,
  WafRule,
} from './types'
import { createUuid } from '../shared/uuid'

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

function isActivationStatus(value: unknown): value is ActivationStatus {
  return value === 'unknown' || value === 'pending' || value === 'active' || value === 'degraded'
}

function isActivationSummary(value: unknown): value is ActivationSummary {
  return (
    isRecord(value) &&
    isActivationStatus(value.status) &&
    ['targetCount', 'activeCount', 'pendingCount', 'degradedCount', 'unknownCount'].every(
      (key) =>
        typeof value[key] === 'number' && Number.isSafeInteger(value[key]) && value[key] >= 0,
    ) &&
    (value.evaluatedAt === null || typeof value.evaluatedAt === 'string')
  )
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
    value.schemaVersion !== 5 ||
    !isRecord(value.projectList) ||
    !isRecord(value.projectRoute) ||
    !isRecord(value.grayRules) ||
    !isRecord(value.edgeFunctions)
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
      'maxUpstreamTlsProfiles',
      'maxUpstreamTlsCaPemBytes',
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
    ]) &&
    hasPositiveIntegers(value.edgeFunctions, [
      'maxPayloadBytes',
      'maxFunctions',
      'maxFunctionNameBytes',
      'maxMountsPerFunction',
      'maxGlobsPerSelector',
      'maxGlobBytes',
      'maxScriptPayloadBytes',
      'maxTotalScriptBytes',
      'maxReportsPerExecution',
      'maxCidrDirectivesPerScript',
      'maxCidrsPerDirective',
      'maxCidrBytes',
      'maxInputFieldBytes',
      'maxBodyPhaseBytes',
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
    isActivationStatus(value.activationStatus) &&
    typeof value.createdAt === 'string' &&
    typeof value.updatedAt === 'string'
  )
}

function isProjectRoutesModel(value: unknown): value is ProjectRoutesModel {
  return (
    isRecord(value) &&
    value.schemaVersion === 6 &&
    value.kind === 'project_routes_yaml' &&
    Array.isArray(value.hostAliases) &&
    value.hostAliases.every((alias) => typeof alias === 'string') &&
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
    isActivationStatus(value.activationStatus) &&
    isActivationSummary(value.activation)
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
    isActivationStatus(value.activationStatus) &&
    isActivationSummary(value.activation) &&
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
      headers: { 'Content-Type': 'application/json', 'Idempotency-Key': createUuid() },
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
    { method: 'POST', headers: { 'Idempotency-Key': createUuid() } },
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
        'Idempotency-Key': createUuid(),
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
        'Idempotency-Key': createUuid(),
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

function isActivationInstance(value: unknown): value is ActivationInstanceView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.instanceKey === 'string' &&
    isActivationStatus(value.status) &&
    (value.buildVersion === null || typeof value.buildVersion === 'string') &&
    (value.buildRevision === null || typeof value.buildRevision === 'string') &&
    (value.evidenceRevision === null || typeof value.evidenceRevision === 'string') &&
    (value.routeSnapshotGeneration === null || typeof value.routeSnapshotGeneration === 'string') &&
    (value.routeSnapshotFingerprintSha256 === null ||
      typeof value.routeSnapshotFingerprintSha256 === 'string') &&
    (value.candidateStatus === null || typeof value.candidateStatus === 'string') &&
    (value.candidateErrorCode === null || typeof value.candidateErrorCode === 'string') &&
    (value.activeMd5 === null || typeof value.activeMd5 === 'string') &&
    (value.activeVersion === null || typeof value.activeVersion === 'string') &&
    (value.observedAt === null || typeof value.observedAt === 'string') &&
    (value.expiresAt === null || typeof value.expiresAt === 'string')
  )
}

export async function fetchReleaseActivation(
  releaseId: string,
  cursor: string | null = null,
  signal?: AbortSignal,
): Promise<ActivationInstanceList> {
  const query = new URLSearchParams({ limit: '50' })
  if (cursor) query.set('cursor', cursor)
  return requestJson(
    `/api/releases/${encodeURIComponent(releaseId)}/activation?${query.toString()}`,
    { signal },
    (value): value is ActivationInstanceList =>
      isRecord(value) &&
      value.releaseId === releaseId &&
      isActivationSummary(value.summary) &&
      Array.isArray(value.items) &&
      value.items.every(isActivationInstance) &&
      (value.nextCursor === null || typeof value.nextCursor === 'string'),
  )
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
        'Idempotency-Key': createUuid(),
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
        'Idempotency-Key': createUuid(),
      },
      body: JSON.stringify({ confirmationDomain, reason }),
    },
    isProjectRelease,
  )
}

export async function queueReleasePublication(releaseId: string): Promise<ProjectReleaseView> {
  const result = await requestJson(
    `/api/releases/${encodeURIComponent(releaseId)}/publications`,
    { method: 'POST', headers: { 'Idempotency-Key': createUuid() } },
    (value): value is { jobId: string; state: string; release: ProjectReleaseView } =>
      isRecord(value) &&
      typeof value.jobId === 'string' &&
      typeof value.state === 'string' &&
      isProjectRelease(value.release),
  )
  return result.release
}

// ---------------------------------------------------------------------------
// WAF policy + edge-function releases
// ---------------------------------------------------------------------------

function isWafMode(value: unknown): value is WafMode {
  return value === 'off' || value === 'log' || value === 'block'
}

function isStringList(value: unknown): value is unknown[] {
  return Array.isArray(value) && value.every((item) => typeof item === 'string')
}

function isWafRule(value: unknown): value is WafRule {
  if (!isRecord(value)) return false
  const scopeOk =
    value.scope === undefined ||
    (isRecord(value.scope) &&
      (value.scope.methods === undefined || isStringList(value.scope.methods)) &&
      (value.scope.paths === undefined || isStringList(value.scope.paths)) &&
      (value.scope.sourceCidrs === undefined || isStringList(value.scope.sourceCidrs)))
  return (
    typeof value.id === 'string' &&
    typeof value.name === 'string' &&
    typeof value.group === 'string' &&
    ['critical', 'high', 'medium', 'low'].includes(String(value.severity)) &&
    typeof value.priority === 'number' &&
    (value.action === 'log' || value.action === 'block') &&
    value.phase === 'request_head' &&
    typeof value.match === 'string' &&
    scopeOk
  )
}

function isWafGroup(value: unknown): value is WafGroup {
  if (!isRecord(value) || !isRecord(value.projects)) return false
  const { include, exempt } = value.projects
  return (
    typeof value.id === 'string' &&
    typeof value.name === 'string' &&
    isWafMode(value.mode) &&
    typeof value.priority === 'number' &&
    isStringList(include) &&
    (exempt === undefined ||
      (Array.isArray(exempt) &&
        exempt.every(
          (exemption) =>
            isRecord(exemption) &&
            typeof exemption.project === 'string' &&
            typeof exemption.reason === 'string',
        )))
  )
}

function isWafPolicy(value: unknown): value is WafPolicy {
  return (
    isRecord(value) &&
    value.schemaVersion === 2 &&
    Array.isArray(value.groups) &&
    value.groups.every(isWafGroup) &&
    Array.isArray(value.rules) &&
    value.rules.every(isWafRule) &&
    isRecord(value.projectModes) &&
    Object.values(value.projectModes).every(isWafMode)
  )
}

function isWafPolicyDraft(value: unknown): value is WafPolicyDraftView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    isWafPolicy(value.policy) &&
    typeof value.policySha256 === 'string' &&
    typeof value.lockVersion === 'string' &&
    typeof value.createdAt === 'string' &&
    typeof value.updatedAt === 'string'
  )
}

function isWafEdgeFunctionMountDraft(value: unknown): value is WafEdgeFunctionMountDraft {
  return (
    isRecord(value) &&
    isRecord(value.projects) &&
    isStringList(value.projects.include) &&
    (value.projects.exclude === undefined || isStringList(value.projects.exclude)) &&
    (value.priority === undefined || typeof value.priority === 'number') &&
    (value.ratio === undefined || typeof value.ratio === 'number')
  )
}

function isWafEdgeFunctionDraftFunction(value: unknown): value is WafEdgeFunctionDraftFunction {
  return (
    isRecord(value) &&
    typeof value.name === 'string' &&
    (value.description === undefined || typeof value.description === 'string') &&
    typeof value.enabled === 'boolean' &&
    (value.phase === undefined ||
      value.phase === 'request_head' ||
      value.phase === 'request_body') &&
    typeof value.script === 'string' &&
    Array.isArray(value.mounts) &&
    value.mounts.every(isWafEdgeFunctionMountDraft)
  )
}

function isWafEdgeFunctionsDraft(value: unknown): value is WafEdgeFunctionsDraft {
  return (
    isRecord(value) &&
    value.schemaVersion === 1 &&
    Array.isArray(value.functions) &&
    value.functions.every(isWafEdgeFunctionDraftFunction)
  )
}

function isWafEdgeFunctionsDraftView(value: unknown): value is WafEdgeFunctionsDraftView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    isWafEdgeFunctionsDraft(value.draft) &&
    typeof value.draftSha256 === 'string' &&
    typeof value.lockVersion === 'string' &&
    typeof value.createdAt === 'string' &&
    typeof value.updatedAt === 'string'
  )
}

function isWafEdgeFunctionCompilePreview(
  value: unknown,
): value is WafEdgeFunctionCompilePreviewView {
  return (
    isRecord(value) &&
    typeof value.valid === 'boolean' &&
    Array.isArray(value.issues) &&
    value.issues.every(isWafPolicyIssue) &&
    Array.isArray(value.functions) &&
    value.functions.every(
      (fn) =>
        isRecord(fn) &&
        typeof fn.name === 'string' &&
        (fn.phase === 'request_head' || fn.phase === 'request_body') &&
        typeof fn.scriptSha256 === 'string' &&
        typeof fn.scriptValid === 'boolean' &&
        Array.isArray(fn.errors) &&
        fn.errors.every(
          (error) =>
            isRecord(error) &&
            typeof error.code === 'string' &&
            (error.field === undefined || typeof error.field === 'string') &&
            (error.offset === undefined || typeof error.offset === 'number') &&
            typeof error.message === 'string',
        ),
    ) &&
    (value.validator === null ||
      (isRecord(value.validator) &&
        typeof value.validator.contractVersion === 'number' &&
        typeof value.validator.revision === 'string'))
  )
}

function isWafPolicyIssue(value: unknown): value is WafPolicyIssue {
  return (
    isRecord(value) &&
    typeof value.field === 'string' &&
    typeof value.code === 'string' &&
    typeof value.message === 'string'
  )
}

function isWafFunctionCompilation(value: unknown): value is WafFunctionCompilation {
  return (
    isRecord(value) &&
    typeof value.groupId === 'string' &&
    typeof value.functionName === 'string' &&
    typeof value.script === 'string' &&
    typeof value.scriptSha256 === 'string' &&
    typeof value.mountPriority === 'number' &&
    Array.isArray(value.mounts) &&
    value.mounts.every(
      (mount) => isRecord(mount) && isStringList(mount.include) && isStringList(mount.exclude),
    ) &&
    isStringList(value.ruleOrder) &&
    Array.isArray(value.directives) &&
    value.directives.every(
      (directive) =>
        isRecord(directive) && typeof directive.name === 'string' && isStringList(directive.cidrs),
    )
  )
}

function isWafCompilePreview(value: unknown): value is WafCompilePreviewView {
  if (!isRecord(value)) return false
  const compilation = value.compilation
  const compilationOk =
    compilation === null ||
    (isRecord(compilation) &&
      Array.isArray(compilation.skipped) &&
      compilation.skipped.every(
        (group) =>
          isRecord(group) &&
          typeof group.groupId === 'string' &&
          (group.reason === 'off' || group.reason === 'no-rules'),
      ) &&
      (compilation.kind === 'unmount'
        ? compilation.reason === 'no-rules' || compilation.reason === 'no-mounted-groups'
        : compilation.kind === 'mounted' &&
          Array.isArray(compilation.functions) &&
          compilation.functions.every(isWafFunctionCompilation)))
  return (
    typeof value.valid === 'boolean' &&
    Array.isArray(value.issues) &&
    value.issues.every(isWafPolicyIssue) &&
    compilationOk &&
    (value.scriptValid === null || typeof value.scriptValid === 'boolean') &&
    Array.isArray(value.scriptErrors) &&
    value.scriptErrors.every(
      (error) =>
        isRecord(error) &&
        typeof error.code === 'string' &&
        typeof error.functionName === 'string' &&
        (error.field === undefined || typeof error.field === 'string') &&
        (error.offset === undefined || typeof error.offset === 'number') &&
        (error.line === undefined || typeof error.line === 'number') &&
        (error.column === undefined || typeof error.column === 'number') &&
        typeof error.message === 'string',
    ) &&
    (value.validator === null ||
      (isRecord(value.validator) &&
        typeof value.validator.contractVersion === 'number' &&
        typeof value.validator.revision === 'string'))
  )
}

function isWafReleaseResource(value: unknown): value is WafReleaseResourceView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    (value.kind === 'edge_function_script' || value.kind === 'edge_function_index') &&
    typeof value.dataId === 'string' &&
    typeof value.group === 'string' &&
    typeof value.status === 'string' &&
    (value.verifiedSha256 === null || typeof value.verifiedSha256 === 'string') &&
    (value.verifiedAt === null || typeof value.verifiedAt === 'string')
  )
}

function isWafRelease(value: unknown): value is WafReleaseView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.sequence === 'string' &&
    typeof value.status === 'string' &&
    Array.isArray(value.wafFunctions) &&
    value.wafFunctions.every(
      (fn) => isRecord(fn) && typeof fn.name === 'string' && typeof fn.scriptSha256 === 'string',
    ) &&
    (value.mode === 'mounted' || value.mode === 'unmount') &&
    typeof value.ruleCount === 'number' &&
    typeof value.indexVersion === 'string' &&
    typeof value.description === 'string' &&
    Array.isArray(value.resources) &&
    value.resources.every(isWafReleaseResource) &&
    isRecord(value.publication) &&
    (value.publication.jobId === null || typeof value.publication.jobId === 'string') &&
    (value.publication.state === null || typeof value.publication.state === 'string') &&
    typeof value.activationStatus === 'string' &&
    isActivationSummary(value.activation) &&
    typeof value.createdAt === 'string' &&
    (value.publishedAt === null || typeof value.publishedAt === 'string')
  )
}

function isWafEdgeFunctions(value: unknown): value is WafEdgeFunctionsView {
  if (
    !isRecord(value) ||
    !isRecord(value.index) ||
    typeof value.index.exists !== 'boolean' ||
    (value.index.sha256 !== null && typeof value.index.sha256 !== 'string') ||
    (value.index.md5 !== null && typeof value.index.md5 !== 'string') ||
    (value.index.version !== null && typeof value.index.version !== 'number') ||
    (value.index.parseError !== null && typeof value.index.parseError !== 'string') ||
    !Array.isArray(value.index.functions) ||
    !value.index.functions.every(
      (entry) =>
        isRecord(entry) &&
        typeof entry.name === 'string' &&
        (entry.sha256 === null || typeof entry.sha256 === 'string') &&
        (entry.phase === null ||
          entry.phase === 'request_head' ||
          entry.phase === 'request_body') &&
        Array.isArray(entry.mounts) &&
        entry.mounts.every(
          (mount) =>
            isRecord(mount) &&
            isRecord(mount.projects) &&
            isStringList(mount.projects.include) &&
            isStringList(mount.projects.exclude) &&
            (mount.priority === null || typeof mount.priority === 'number') &&
            (mount.ratio === null || typeof mount.ratio === 'number'),
        ),
    )
  ) {
    return false
  }
  return (
    Array.isArray(value.scripts) &&
    value.scripts.every(
      (script) =>
        isRecord(script) &&
        typeof script.name === 'string' &&
        typeof script.exists === 'boolean' &&
        (script.sha256 === null || typeof script.sha256 === 'string') &&
        (script.content === null || typeof script.content === 'string'),
    )
  )
}

function isWafRulePack(value: unknown): value is WafRulePackView {
  return (
    isRecord(value) &&
    typeof value.version === 'string' &&
    Array.isArray(value.groups) &&
    value.groups.every(isWafGroup) &&
    Array.isArray(value.rules) &&
    value.rules.every(isWafRule)
  )
}

export async function fetchWafRulePack(signal?: AbortSignal): Promise<WafRulePackView> {
  return requestJson('/api/waf/rule-pack', { signal }, isWafRulePack)
}

function isWafHits(value: unknown): value is WafHitsView {
  return (
    isRecord(value) &&
    (value.window === '1h' || value.window === '24h' || value.window === '7d') &&
    typeof value.queriedAt === 'string' &&
    Array.isArray(value.rows) &&
    value.rows.every(
      (row) =>
        isRecord(row) &&
        typeof row.ruleId === 'string' &&
        typeof row.event === 'string' &&
        typeof row.count === 'number',
    )
  )
}

export async function fetchWafHits(
  window: WafHitsWindow,
  signal?: AbortSignal,
): Promise<WafHitsView> {
  return requestJson(`/api/waf/hits?window=${window}`, { signal }, isWafHits)
}

export async function fetchWafPolicyDraft(signal?: AbortSignal): Promise<WafPolicyDraftView> {
  return requestJson('/api/waf/policy', { signal }, isWafPolicyDraft)
}

export async function createWafPolicyDraft(): Promise<WafPolicyDraftView> {
  return requestJson('/api/waf/policy', { method: 'POST' }, isWafPolicyDraft)
}

export async function saveWafPolicyDraft(
  policy: WafPolicy,
  lockVersion: string,
): Promise<WafPolicyDraftView> {
  return requestJson(
    '/api/waf/policy',
    {
      method: 'PUT',
      headers: {
        'Content-Type': 'application/json',
        'If-Match': `"${lockVersion}"`,
      },
      body: JSON.stringify({ policy }),
    },
    isWafPolicyDraft,
  )
}

export async function compileWafPolicyPreview(
  policy: WafPolicy,
  signal?: AbortSignal,
): Promise<WafCompilePreviewView> {
  return requestJson(
    '/api/waf/policy/compile-preview',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(policy),
      signal,
    },
    isWafCompilePreview,
  )
}

export async function fetchWafReleases(signal?: AbortSignal): Promise<readonly WafReleaseView[]> {
  const result = await requestJson(
    '/api/waf/releases',
    { signal },
    (value): value is { items: WafReleaseView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isWafRelease),
  )
  return result.items
}

export async function createWafRelease(): Promise<WafReleaseView> {
  return requestJson(
    '/api/waf/releases',
    { method: 'POST', headers: { 'Idempotency-Key': createUuid() } },
    isWafRelease,
  )
}

export async function rollbackWafRelease(releaseId: string): Promise<WafReleaseView> {
  return requestJson(
    `/api/waf/releases/${encodeURIComponent(releaseId)}/rollback`,
    { method: 'POST', headers: { 'Idempotency-Key': createUuid() } },
    isWafRelease,
  )
}

export async function queueWafReleasePublication(releaseId: string): Promise<WafReleaseView> {
  const result = await requestJson(
    `/api/waf/releases/${encodeURIComponent(releaseId)}/publication`,
    { method: 'POST', headers: { 'Idempotency-Key': createUuid() } },
    (value): value is { jobId: string; state: string; release: WafReleaseView } =>
      isRecord(value) &&
      typeof value.jobId === 'string' &&
      typeof value.state === 'string' &&
      isWafRelease(value.release),
  )
  return result.release
}

export async function fetchWafEdgeFunctions(signal?: AbortSignal): Promise<WafEdgeFunctionsView> {
  return requestJson('/api/waf/edge-functions', { signal }, isWafEdgeFunctions)
}

export async function fetchWafEdgeFunctionsDraft(
  signal?: AbortSignal,
): Promise<WafEdgeFunctionsDraftView> {
  return requestJson('/api/waf/edge-functions/draft', { signal }, isWafEdgeFunctionsDraftView)
}

export async function createWafEdgeFunctionsDraft(): Promise<WafEdgeFunctionsDraftView> {
  return requestJson(
    '/api/waf/edge-functions/draft',
    { method: 'POST' },
    isWafEdgeFunctionsDraftView,
  )
}

export async function saveWafEdgeFunctionsDraft(
  draft: WafEdgeFunctionsDraft,
  lockVersion: string,
): Promise<WafEdgeFunctionsDraftView> {
  return requestJson(
    '/api/waf/edge-functions/draft',
    {
      method: 'PUT',
      headers: {
        'Content-Type': 'application/json',
        'If-Match': `"${lockVersion}"`,
      },
      body: JSON.stringify({ draft }),
    },
    isWafEdgeFunctionsDraftView,
  )
}

export async function compileWafEdgeFunctionsPreview(
  draft: WafEdgeFunctionsDraft,
  signal?: AbortSignal,
): Promise<WafEdgeFunctionCompilePreviewView> {
  return requestJson(
    '/api/waf/edge-functions/compile-preview',
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(draft),
      signal,
    },
    isWafEdgeFunctionCompilePreview,
  )
}
