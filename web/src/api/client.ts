import type {
  DraftRevisionView,
  EnvironmentView,
  HealthResponse,
  ProjectRouteModel,
  ProjectView,
  SystemStatusResponse,
} from './types'

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

function isSystemStatus(value: unknown): value is SystemStatusResponse {
  if (!isRecord(value) || !isRecord(value.dependencies)) return false
  return (
    (value.status === 'ready' || value.status === 'degraded') &&
    value.service === 'access-gateway-console-api' &&
    isRecord(value.dependencies.database) &&
    typeof value.dependencies.database.status === 'string'
  )
}

function isEnvironment(value: unknown): value is EnvironmentView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.code === 'string' &&
    typeof value.name === 'string' &&
    typeof value.tier === 'string' &&
    isRecord(value.nacos) &&
    typeof value.nacos.endpoint === 'string'
  )
}

function isProject(value: unknown): value is ProjectView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.environmentId === 'string' &&
    typeof value.domain === 'string' &&
    typeof value.status === 'string'
  )
}

function isProjectRouteModel(value: unknown): value is ProjectRouteModel {
  return (
    isRecord(value) &&
    value.schemaVersion === 1 &&
    value.kind === 'project_route' &&
    Array.isArray(value.hosts) &&
    Array.isArray(value.routes)
  )
}

function isDraftRevision(value: unknown): value is DraftRevisionView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.draftId === 'string' &&
    typeof value.revision === 'number' &&
    isProjectRouteModel(value.model) &&
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
    throw new Error(apiErrorMessage(body, response.status))
  }
  if (!validate(body)) {
    throw new Error('Console API returned an invalid response')
  }
  return body
}

export async function fetchSystemStatus(signal?: AbortSignal): Promise<SystemStatusResponse> {
  return requestJson('/api/system/status', { signal }, isSystemStatus)
}

export async function fetchWorkspace(signal?: AbortSignal): Promise<EnvironmentView> {
  return requestJson('/api/workspace', { signal }, isEnvironment)
}

export async function fetchProjects(
  environmentId: string,
  signal?: AbortSignal,
): Promise<readonly ProjectView[]> {
  const response = await requestJson(
    `/api/environments/${encodeURIComponent(environmentId)}/projects`,
    { signal },
    (value): value is { items: ProjectView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isProject),
  )
  return response.items
}

export async function createProject(
  environmentId: string,
  domain: string,
): Promise<{ id: string }> {
  return requestJson(
    `/api/environments/${encodeURIComponent(environmentId)}/projects`,
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
  model: ProjectRouteModel,
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
