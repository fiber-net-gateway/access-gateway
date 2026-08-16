import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import type { AddressInfo } from 'node:net'
import test from 'node:test'

import { HttpActivationEvidenceClient } from './http.js'
import { ActivationEvidenceClientError } from './model.js'

function page(name: string, nextCursor: string | null) {
  const resource = {
    dataId: 'ploto.unified-access.projects',
    group: 'ACCESS-SERVER',
    candidateStatus: 'accepted',
    observedMd5: '11111111111111111111111111111111',
    activeMd5: '11111111111111111111111111111111',
    observedAtUnixMillis: 1,
    activeAtUnixMillis: 1,
    failure: null,
  }
  return {
    contractVersion: 1,
    evidenceRevision: '7',
    instance: {
      id: 'access-0',
      buildVersion: '0.1.0',
      buildRevision: 'a'.repeat(40),
      startedAtUnixMillis: 1,
    },
    runtime: { state: 'running' },
    routeSnapshot: {
      generation: '3',
      fingerprintSha256: 'b'.repeat(64),
      publishedAtUnixMillis: 1,
      publicationMode: 'atomic_request_pin',
    },
    accessConfig: {
      watcherState: 'running',
      readinessState: 'ready',
      projectList: resource,
    },
    projects: {
      items: [
        {
          name,
          dataId: `ploto.unified-access.route.${name}`,
          group: 'ACCESS-SERVER',
          subscriptionState: 'subscribed',
          candidateStatus: 'accepted',
          observedMd5: '22222222222222222222222222222222',
          observedVersion: -2,
          activeMd5: '22222222222222222222222222222222',
          activeVersion: -2,
          activeSnapshotGeneration: '3',
          activeLoaded: true,
          observedAtUnixMillis: 1,
          activeAtUnixMillis: 1,
          failure: null,
        },
      ],
      nextCursor,
    },
    gray: { watcherState: 'running', resource, generation: '1', ruleCount: 0 },
    tls: { enabled: false, watcherState: 'disabled', resource, version: '0', certificateCount: 0 },
    discovery: {
      clientState: 'running',
      configServiceState: 'running',
      namingServiceState: 'running',
      readyServices: 1,
      selectableEndpoints: 2,
      logicalClusters: 1,
      selectorLeases: 0,
    },
  }
}

test('activation evidence client authenticates and pins all paginated data to one revision', async (context) => {
  const requests: { authorization: string | undefined; url: string }[] = []
  const server = createServer((request, response) => {
    requests.push({
      authorization: request.headers.authorization,
      url: request.url ?? '',
    })
    const second = (request.url ?? '').includes('cursor=7%3A1')
    response.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' })
    response.end(
      JSON.stringify(page(second ? 'b.example.com' : 'a.example.com', second ? null : '7:1')),
    )
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  context.after(() => new Promise<void>((resolve) => server.close(() => resolve())))
  const address = server.address() as AddressInfo
  const token = '0123456789abcdef0123456789abcdef'
  const client = new HttpActivationEvidenceClient({
    timeoutMillis: 2_000,
    maxResponseBytes: 64 * 1024,
    maxPages: 4,
    maxProjects: 8,
  })

  const result = await client.collect({
    environmentCode: 'demo',
    instanceKey: 'access-0',
    endpoint: `http://127.0.0.1:${address.port}/v1/activation-evidence`,
    token,
  })

  assert.deepEqual(
    result.projects.map((project) => project.name),
    ['a.example.com', 'b.example.com'],
  )
  assert.equal(requests.length, 2)
  assert.equal(result.projects[0]?.activeVersion, -2)
  assert.equal(requests[0]?.authorization, `Bearer ${token}`)
  assert.match(requests[0]?.url ?? '', /limit=256/u)
  assert.match(requests[1]?.url ?? '', /cursor=7%3A1/u)
})

test('activation evidence client restarts a traversal once when the pinned revision changes', async (context) => {
  let requests = 0
  const server = createServer((request, response) => {
    requests += 1
    if (requests === 2) {
      response.writeHead(409, { 'Content-Type': 'application/json' })
      response.end('{"error":"evidence_changed"}')
      return
    }
    const second = (request.url ?? '').includes('cursor=7%3A1')
    response.writeHead(200, { 'Content-Type': 'application/json' })
    response.end(
      JSON.stringify(page(second ? 'b.example.com' : 'a.example.com', second ? null : '7:1')),
    )
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  context.after(() => new Promise<void>((resolve) => server.close(() => resolve())))
  const address = server.address() as AddressInfo
  const client = new HttpActivationEvidenceClient({
    timeoutMillis: 2_000,
    maxResponseBytes: 64 * 1024,
    maxPages: 4,
    maxProjects: 8,
  })

  const result = await client.collect({
    environmentCode: 'demo',
    instanceKey: 'access-0',
    endpoint: `http://127.0.0.1:${address.port}/v1/activation-evidence`,
    token: '0123456789abcdef0123456789abcdef',
  })

  assert.deepEqual(
    result.projects.map((project) => project.name),
    ['a.example.com', 'b.example.com'],
  )
  assert.equal(requests, 4)
})

test('activation evidence client rejects an out-of-range evidence revision', async (context) => {
  const server = createServer((_request, response) => {
    const payload = page('duplicate.example.com', null)
    payload.evidenceRevision = '18446744073709551616'
    response.writeHead(200, { 'Content-Type': 'application/json' })
    response.end(JSON.stringify(payload))
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  context.after(() => new Promise<void>((resolve) => server.close(() => resolve())))
  const address = server.address() as AddressInfo
  const client = new HttpActivationEvidenceClient({
    timeoutMillis: 2_000,
    maxResponseBytes: 64 * 1024,
    maxPages: 4,
    maxProjects: 8,
  })

  await assert.rejects(
    client.collect({
      environmentCode: 'demo',
      instanceKey: 'access-0',
      endpoint: `http://127.0.0.1:${address.port}/v1/activation-evidence`,
      token: '0123456789abcdef0123456789abcdef',
    }),
    (error: unknown) =>
      error instanceof ActivationEvidenceClientError &&
      error.code === 'ACTIVATION_INVALID_RESPONSE',
  )
})

test('activation evidence client rejects unknown typed lifecycle states', async (context) => {
  const server = createServer((_request, response) => {
    const payload = page('typed.example.com', null)
    payload.accessConfig.watcherState = 'invented_state'
    response.writeHead(200, { 'Content-Type': 'application/json' })
    response.end(JSON.stringify(payload))
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  context.after(() => new Promise<void>((resolve) => server.close(() => resolve())))
  const address = server.address() as AddressInfo
  const client = new HttpActivationEvidenceClient({
    timeoutMillis: 2_000,
    maxResponseBytes: 64 * 1024,
    maxPages: 4,
    maxProjects: 8,
  })

  await assert.rejects(
    client.collect({
      environmentCode: 'demo',
      instanceKey: 'access-0',
      endpoint: `http://127.0.0.1:${address.port}/v1/activation-evidence`,
      token: '0123456789abcdef0123456789abcdef',
    }),
    (error: unknown) =>
      error instanceof ActivationEvidenceClientError &&
      error.code === 'ACTIVATION_INVALID_RESPONSE',
  )
})

test('activation evidence client rejects duplicate Projects in a complete traversal', async (context) => {
  const server = createServer((_request, response) => {
    const payload = page('duplicate.example.com', null)
    payload.projects.items.push({ ...payload.projects.items[0]! })
    response.writeHead(200, { 'Content-Type': 'application/json' })
    response.end(JSON.stringify(payload))
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  context.after(() => new Promise<void>((resolve) => server.close(() => resolve())))
  const address = server.address() as AddressInfo
  const client = new HttpActivationEvidenceClient({
    timeoutMillis: 2_000,
    maxResponseBytes: 64 * 1024,
    maxPages: 4,
    maxProjects: 8,
  })

  await assert.rejects(
    client.collect({
      environmentCode: 'demo',
      instanceKey: 'access-0',
      endpoint: `http://127.0.0.1:${address.port}/v1/activation-evidence`,
      token: '0123456789abcdef0123456789abcdef',
    }),
    (error: unknown) =>
      error instanceof ActivationEvidenceClientError &&
      error.code === 'ACTIVATION_INVALID_RESPONSE',
  )
})
