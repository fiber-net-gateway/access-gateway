import type { HealthResponse } from './types'

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
  const response = await fetch('/api/health', {
    headers: { Accept: 'application/json' },
    signal,
  })

  if (!response.ok) {
    throw new Error(`Console API health check failed with status ${response.status}`)
  }

  const body: unknown = await response.json()
  if (!isHealthResponse(body)) {
    throw new Error('Console API returned an invalid health response')
  }
  return body
}
