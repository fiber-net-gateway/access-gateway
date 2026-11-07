// Minimal fail-closed client for the Prometheus HTTP query API (v1).
//
// The console only ever issues read-only instant queries (the WAF hit
// statistics page); credentials, service discovery, and ranges stay out of
// scope. Every malformed response is rejected loudly instead of guessed at —
// an unparseable body never surfaces as "zero hits".

import { unavailable } from '../../shared/errors.js'

export interface PrometheusInstantSample {
  /** Label set of the series, e.g. {event: 'waf.block', label: 'sqli-…'}. */
  labels: Readonly<Record<string, string>>
  value: number
  /** Evaluation timestamp from the response, in milliseconds since epoch. */
  timestampMillis: number
}

export interface PrometheusQueryClient {
  readonly available: boolean
  /** Runs an instant query and returns the (possibly empty) vector. */
  queryInstant(promql: string): Promise<readonly PrometheusInstantSample[]>
}

export interface HttpPrometheusQueryClientOptions {
  endpoint: string
  timeoutMillis: number
  maxResponseBytes: number
}

interface PrometheusVectorItem {
  metric?: unknown
  value?: unknown
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null
}

function parseLabels(value: unknown): Record<string, string> | null {
  if (!isRecord(value)) return null
  const labels: Record<string, string> = {}
  for (const [key, item] of Object.entries(value)) {
    if (typeof item !== 'string') return null
    labels[key] = item
  }
  return labels
}

// Defensively narrows a Prometheus v1 instant-query response. Anything that
// does not match the documented shape (status success, vector result, sample
// value pairs) is an error, never a silently empty result.
function parseInstantVector(body: unknown): readonly PrometheusInstantSample[] {
  if (!isRecord(body) || body.status !== 'success' || !isRecord(body.data)) {
    throw unavailable('PROMETHEUS_QUERY_FAILED', 'Prometheus returned an unexpected status')
  }
  if (body.data.resultType !== 'vector' || !Array.isArray(body.data.result)) {
    throw unavailable('PROMETHEUS_QUERY_FAILED', 'Prometheus returned an unexpected result type')
  }
  const samples: PrometheusInstantSample[] = []
  for (const item of body.data.result as unknown[]) {
    const vectorItem = item as PrometheusVectorItem
    const labels = parseLabels(vectorItem.metric)
    if (!labels || !Array.isArray(vectorItem.value) || vectorItem.value.length < 2) {
      throw unavailable('PROMETHEUS_QUERY_FAILED', 'Prometheus returned a malformed sample')
    }
    const [timestamp, rawValue] = vectorItem.value as unknown[]
    const value = typeof rawValue === 'string' ? Number(rawValue) : Number.NaN
    if (typeof timestamp !== 'number' || !Number.isFinite(value)) {
      throw unavailable('PROMETHEUS_QUERY_FAILED', 'Prometheus returned a malformed sample value')
    }
    samples.push({ labels, value, timestampMillis: Math.round(timestamp * 1000) })
  }
  return samples
}

export class HttpPrometheusQueryClient implements PrometheusQueryClient {
  readonly available = true
  readonly #endpoint: string
  readonly #timeoutMillis: number
  readonly #maxResponseBytes: number

  constructor(options: HttpPrometheusQueryClientOptions) {
    this.#endpoint = options.endpoint
    this.#timeoutMillis = options.timeoutMillis
    this.#maxResponseBytes = options.maxResponseBytes
  }

  async queryInstant(promql: string): Promise<readonly PrometheusInstantSample[]> {
    const url = new URL('/api/v1/query', this.#endpoint)
    url.search = new URLSearchParams({ query: promql }).toString()
    let response: Response
    try {
      response = await fetch(url, {
        headers: { Accept: 'application/json' },
        signal: AbortSignal.timeout(this.#timeoutMillis),
      })
    } catch {
      throw unavailable('PROMETHEUS_QUERY_UNAVAILABLE', 'Prometheus query request failed')
    }
    if (!response.ok) {
      throw unavailable('PROMETHEUS_QUERY_FAILED', 'Prometheus rejected the query')
    }
    const text = await response.text()
    if (Buffer.byteLength(text, 'utf8') > this.#maxResponseBytes) {
      throw unavailable('PROMETHEUS_RESPONSE_LIMIT', 'Prometheus response exceeded the size limit')
    }
    let body: unknown
    try {
      body = JSON.parse(text)
    } catch {
      throw unavailable('PROMETHEUS_QUERY_FAILED', 'Prometheus returned a non-JSON body')
    }
    return parseInstantVector(body)
  }
}

export class UnavailablePrometheusQueryClient implements PrometheusQueryClient {
  readonly available = false

  async queryInstant(): Promise<never> {
    throw unavailable('PROMETHEUS_UNCONFIGURED', 'Prometheus is not configured')
  }
}
