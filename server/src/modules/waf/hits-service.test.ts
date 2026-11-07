import assert from 'node:assert/strict'
import test from 'node:test'

import type {
  PrometheusInstantSample,
  PrometheusQueryClient,
} from '../../integrations/prometheus/http.js'
import { UnavailablePrometheusQueryClient } from '../../integrations/prometheus/http.js'
import type { Actor } from '../auth/model.js'
import {
  DefaultWafHitsService,
  UnavailableWafHitsService,
  isWafHitsWindow,
  type WafHitsService,
  type WafHitsWindow,
} from './hits-service.js'

const actor: Actor = {
  internalId: '0',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'tester',
  displayName: 'Tester',
  platformAdmin: true,
}

function fakeQueryClient(samples: readonly PrometheusInstantSample[]): PrometheusQueryClient & {
  queries: string[]
} {
  const queries: string[] = []
  return {
    available: true,
    queries,
    async queryInstant(promql: string) {
      queries.push(promql)
      return samples
    },
  }
}

function sample(event: string, ruleId: string, value: number): PrometheusInstantSample {
  return { labels: { event, label: ruleId }, value, timestampMillis: 0 }
}

test('hits service asks Prometheus for the waf event increases over the window', async () => {
  const client = fakeQueryClient([
    sample('waf.log', 'scanner-user-agent', 40),
    sample('waf.block', 'sqli-union-select', 7),
  ])
  const service = new DefaultWafHitsService(client)

  const view = await service.hits(actor, '24h')

  assert.equal(client.queries.length, 1)
  assert.equal(
    client.queries[0],
    'sum by (event, label) (increase(access_server_function_events_total{function=~"waf(-.+)?"}[24h]))',
  )
  assert.equal(view.window, '24h')
  assert.ok(!Number.isNaN(Date.parse(view.queriedAt)))
  assert.deepEqual(view.rows, [
    { ruleId: 'scanner-user-agent', event: 'waf.log', count: 40 },
    { ruleId: 'sqli-union-select', event: 'waf.block', count: 7 },
  ])
})

test('hits service drops malformed samples and sorts by count', async () => {
  const client = fakeQueryClient([
    sample('waf.log', 'probe-sensitive-file', 3),
    // Missing label / event, oversize or negative values: dropped, not guessed.
    { labels: { event: 'waf.log' }, value: 5, timestampMillis: 0 },
    { labels: { label: 'orphan-rule' }, value: 5, timestampMillis: 0 },
    { labels: { event: 'waf.block', label: 'x'.repeat(65) }, value: 9, timestampMillis: 0 },
    { labels: { event: 'waf.block', label: 'negative-rule' }, value: -1, timestampMillis: 0 },
    sample('waf.block', 'sqli-time-function', 12),
  ])
  const service = new DefaultWafHitsService(client)

  const view = await service.hits(actor, '1h')

  assert.deepEqual(view.rows, [
    { ruleId: 'sqli-time-function', event: 'waf.block', count: 12 },
    { ruleId: 'probe-sensitive-file', event: 'waf.log', count: 3 },
  ])
})

test('hits service caps the row list to keep responses bounded', async () => {
  const samples = Array.from({ length: 600 }, (_, index) =>
    sample('waf.log', `rule-${index}`, index),
  )
  const service = new DefaultWafHitsService(fakeQueryClient(samples))

  const view = await service.hits(actor, '7d')

  assert.equal(view.rows.length, 512)
  // Highest counts survive the cap.
  assert.equal(view.rows[0]?.ruleId, 'rule-599')
})

test('hits service fails closed when Prometheus is unavailable or unconfigured', async () => {
  const configured = new DefaultWafHitsService(new UnavailablePrometheusQueryClient())
  await assert.rejects(
    () => configured.hits(actor, '24h'),
    (error: unknown) => {
      assert.equal((error as { code: string }).code, 'PROMETHEUS_UNCONFIGURED')
      return true
    },
  )
  const unavailable: WafHitsService = new UnavailableWafHitsService()
  await assert.rejects(
    () => unavailable.hits(actor, '24h'),
    (error: unknown) => {
      assert.equal((error as { code: string }).code, 'PROMETHEUS_UNCONFIGURED')
      return true
    },
  )
})

test('hit windows are the closed enum used by the route', () => {
  const windows: WafHitsWindow[] = ['1h', '24h', '7d']
  for (const window of windows) assert.equal(isWafHitsWindow(window), true)
  assert.equal(isWafHitsWindow('30m'), false)
  assert.equal(isWafHitsWindow('24h]; drop_series(1'), false)
})
