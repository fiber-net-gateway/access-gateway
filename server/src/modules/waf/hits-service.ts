// WAF hit statistics (design §3.7 / P3 ⑤): the data plane counts every rule
// match as `access_server_function_events_total{function="waf-<group>", event,
// label=<ruleId>}`; this service asks Prometheus for the per-rule increases
// over a window. The data plane stays the source of truth — the console only
// aggregates what Prometheus already scraped.

import { unavailable } from '../../shared/errors.js'
import type { PrometheusQueryClient } from '../../integrations/prometheus/http.js'
import type { Actor } from '../auth/model.js'

export const WAF_HITS_WINDOWS = ['1h', '24h', '7d'] as const
export type WafHitsWindow = (typeof WAF_HITS_WINDOWS)[number]

export function isWafHitsWindow(value: string): value is WafHitsWindow {
  return (WAF_HITS_WINDOWS as readonly string[]).includes(value)
}

export interface WafHitRow {
  ruleId: string
  /** Event name reported by the generated script: waf.log or waf.block. */
  event: string
  count: number
}

export interface WafHitsView {
  window: WafHitsWindow
  queriedAt: string
  rows: readonly WafHitRow[]
}

export interface WafHitsService {
  hits(actor: Actor, window: WafHitsWindow): Promise<WafHitsView>
}

// Generated functions all match this selector, and a window reaching back past
// rule groups still sees the old single `waf` function. Rule ids are unique
// across groups, so summing over `function` keeps a moved rule on one row. The
// rule id travels in the `label` label. `window` is enum-typed upstream, so the
// query string below is always well-formed PromQL.
function hitsQuery(window: WafHitsWindow): string {
  return `sum by (event, label) (increase(access_server_function_events_total{function=~"waf(-.+)?"}[${window}]))`
}

// Wire rule ids are ≤64 bytes (validator schema) and events ≤32; anything
// longer than the data plane could ever report is dropped defensively rather
// than surfaced as a rule id.
const maxRuleIdBytes = 64
const maxEventBytes = 32
const maxRows = 512

export class DefaultWafHitsService implements WafHitsService {
  readonly #prometheus: PrometheusQueryClient

  constructor(prometheus: PrometheusQueryClient) {
    this.#prometheus = prometheus
  }

  async hits(_actor: Actor, window: WafHitsWindow): Promise<WafHitsView> {
    if (!this.#prometheus.available) {
      throw unavailable('PROMETHEUS_UNCONFIGURED', 'Prometheus is not configured')
    }
    const samples = await this.#prometheus.queryInstant(hitsQuery(window))
    const rows: WafHitRow[] = []
    for (const sample of samples) {
      const ruleId = sample.labels.label
      const event = sample.labels.event
      if (
        ruleId === undefined ||
        event === undefined ||
        ruleId.length === 0 ||
        Buffer.byteLength(ruleId, 'utf8') > maxRuleIdBytes ||
        event.length === 0 ||
        Buffer.byteLength(event, 'utf8') > maxEventBytes ||
        !Number.isFinite(sample.value) ||
        sample.value < 0
      ) {
        continue
      }
      rows.push({ ruleId, event, count: sample.value })
    }
    rows.sort((a, b) => b.count - a.count || a.ruleId.localeCompare(b.ruleId))
    return {
      window,
      queriedAt: new Date().toISOString(),
      rows: rows.length > maxRows ? rows.slice(0, maxRows) : rows,
    }
  }
}

export class UnavailableWafHitsService implements WafHitsService {
  async hits(): Promise<never> {
    throw unavailable('PROMETHEUS_UNCONFIGURED', 'Prometheus is not configured')
  }
}
