import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import type { AddressInfo } from 'node:net'
import test from 'node:test'

import { HttpNacosClient } from './http.js'

test('HTTP Nacos client applies the deployment endpoint and verifies exact content', async (context) => {
  const requests: { method: string; url: string; body: string }[] = []
  const server = createServer((request, response) => {
    let body = ''
    request.setEncoding('utf8')
    request.on('data', (chunk) => {
      body += chunk
    })
    request.on('end', () => {
      requests.push({ method: request.method ?? '', url: request.url ?? '', body })
      response.writeHead(200, { 'Content-Type': 'text/plain' })
      response.end(request.method === 'POST' ? 'true' : 'route-content')
    })
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  context.after(() => new Promise<void>((resolve) => server.close(() => resolve())))
  const address = server.address() as AddressInfo
  const client = new HttpNacosClient({
    timeoutMillis: 2_000,
    maxResponseBytes: 1_024,
    endpointOverride: `http://127.0.0.1:${address.port}`,
  })
  const target = {
    endpoint: 'http://127.0.0.1:1',
    namespace: 'public',
    tenant: '',
    credentialConfigured: false,
  }

  const value = await client.read(target, 'route.demo', 'ACCESS-SERVER')
  assert.equal(value.exists, true)
  assert.equal(value.content, 'route-content')
  assert.match(value.sha256 ?? '', /^[0-9a-f]{64}$/u)
  await client.write(target, 'route.demo', 'ACCESS-SERVER', 'next-content', 'json')

  assert.equal(requests.length, 2)
  assert.equal(requests[0]?.method, 'GET')
  assert.match(requests[0]?.url ?? '', /dataId=route\.demo/u)
  assert.match(requests[0]?.url ?? '', /group=ACCESS-SERVER/u)
  assert.equal(requests[1]?.method, 'POST')
  assert.match(requests[1]?.body ?? '', /content=next-content/u)
  assert.match(requests[1]?.body ?? '', /type=json/u)
})
