import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import type { AddressInfo } from 'node:net'
import test from 'node:test'

import {
  HttpPrometheusQueryClient,
  UnavailablePrometheusQueryClient,
  type PrometheusQueryClient,
} from './http.js'

function vectorBody(samples: unknown[]): string {
  return JSON.stringify({ status: 'success', data: { resultType: 'vector', result: samples } })
}

async function startServer(
  handler: (request: { url?: string }, response: import('node:http').ServerResponse) => void,
): Promise<{ client: HttpPrometheusQueryClient; endpoint: string; urls: string[] }> {
  const urls: string[] = []
  const server = createServer((request, response) => {
    urls.push(request.url ?? '')
    handler(request, response)
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  test.after(() => new Promise<void>((resolve) => server.close(() => resolve())))
  const address = server.address() as AddressInfo
  const endpoint = `http://127.0.0.1:${address.port}`
  const client = new HttpPrometheusQueryClient({
    endpoint,
    timeoutMillis: 2_000,
    maxResponseBytes: 65_536,
  })
  return { client, endpoint, urls }
}

test('prometheus client parses instant vectors and forwards the query verbatim', async () => {
  const { client, urls } = await startServer((_request, response) => {
    response.writeHead(200, { 'Content-Type': 'application/json' })
    response.end(
      vectorBody([
        {
          metric: { event: 'waf.block', label: 'sqli-union-select' },
          value: [1699344000.0, '17'],
        },
      ]),
    )
  })

  const samples = await client.queryInstant('sum by (event, label) (increase(m[24h]))')

  assert.equal(samples.length, 1)
  assert.deepEqual(samples[0]?.labels, { event: 'waf.block', label: 'sqli-union-select' })
  assert.equal(samples[0]?.value, 17)
  assert.equal(samples[0]?.timestampMillis, 1699344000000)
  assert.match(urls[0] ?? '', /^\/api\/v1\/query\?/u)
  const query = new URL(urls[0] ?? '', 'http://localhost').searchParams.get('query')
  assert.equal(query, 'sum by (event, label) (increase(m[24h]))')
})

test('prometheus client rejects query failures, malformed bodies, and oversized responses', async () => {
  const cases: { status: number; body: string }[] = [
    { status: 500, body: 'boom' },
    { status: 200, body: 'not json' },
    { status: 200, body: JSON.stringify({ status: 'error', data: null }) },
    { status: 200, body: vectorBody([{ metric: { event: 'waf.block' }, value: [1, 'x'] }]) },
    { status: 200, body: vectorBody([{ metric: 'nope', value: [1, '2'] }]) },
    { status: 200, body: JSON.stringify({ status: 'success', data: { resultType: 'matrix' } }) },
  ]
  for (const item of cases) {
    const { client } = await startServer((_request, response) => {
      response.writeHead(item.status, { 'Content-Type': 'application/json' })
      response.end(item.body)
    })
    await assert.rejects(
      () => client.queryInstant('up'),
      (error: unknown) => {
        assert.match(
          (error as { code: string }).code,
          /^PROMETHEUS_(QUERY_FAILED|QUERY_UNAVAILABLE)$/,
        )
        return true
      },
    )
  }

  const { endpoint } = await startServer((_request, response) => {
    response.writeHead(200, { 'Content-Type': 'application/json' })
    response.end(vectorBody([]))
  })
  const limited = new HttpPrometheusQueryClient({
    endpoint,
    timeoutMillis: 2_000,
    maxResponseBytes: 1,
  })
  await assert.rejects(
    () => limited.queryInstant('up'),
    (error: unknown) => {
      assert.equal((error as { code: string }).code, 'PROMETHEUS_RESPONSE_LIMIT')
      return true
    },
  )
})

test('prometheus client accepts an empty vector as a valid empty result', async () => {
  const { client } = await startServer((_request, response) => {
    response.writeHead(200, { 'Content-Type': 'application/json' })
    response.end(vectorBody([]))
  })

  assert.deepEqual(await client.queryInstant('up'), [])
})

test('unavailable prometheus client fails closed', async () => {
  const client: PrometheusQueryClient = new UnavailablePrometheusQueryClient()
  assert.equal(client.available, false)
  await assert.rejects(
    () => client.queryInstant('up'),
    (error: unknown) => {
      assert.equal((error as { code: string }).code, 'PROMETHEUS_UNCONFIGURED')
      return true
    },
  )
})
