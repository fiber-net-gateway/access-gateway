import type {
  DraftRevisionView,
  HealthResponse,
  ProjectRoutesModel,
  ProjectRoutesValidationView,
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

function isProject(value: unknown): value is ProjectView {
  return (
    isRecord(value) &&
    typeof value.id === 'string' &&
    typeof value.domain === 'string' &&
    typeof value.status === 'string'
  )
}

function isProjectRoutesModel(value: unknown): value is ProjectRoutesModel {
  return (
    isRecord(value) &&
    value.schemaVersion === 2 &&
    value.kind === 'project_routes_yaml' &&
    Array.isArray(value.routes) &&
    value.routes.every(
      (route) =>
        isRecord(route) && typeof route.id === 'string' && typeof route.source === 'string',
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

export async function fetchProjects(signal?: AbortSignal): Promise<readonly ProjectView[]> {
  const response = await requestJson(
    '/api/projects',
    { signal },
    (value): value is { items: ProjectView[] } =>
      isRecord(value) && Array.isArray(value.items) && value.items.every(isProject),
  )
  return response.items
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
