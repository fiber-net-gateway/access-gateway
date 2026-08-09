const apiUrl = process.env.CONSOLE_API_URL ?? 'http://console-api:3000'
const nacosEndpoint = process.env.DEMO_NACOS_ENDPOINT ?? 'http://localhost:8848'

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds))

async function request(path, init = {}) {
  const response = await fetch(`${apiUrl}${path}`, {
    ...init,
    headers: {
      Accept: 'application/json',
      ...(init.body ? { 'Content-Type': 'application/json' } : {}),
      ...init.headers,
    },
  })
  const body = await response.json().catch(() => null)
  if (!response.ok) {
    throw new Error(`Console API ${init.method ?? 'GET'} ${path} failed (${response.status})`)
  }
  return body
}

async function waitForConsole() {
  for (let attempt = 0; attempt < 60; attempt += 1) {
    try {
      await request('/api/health/ready')
      return
    } catch {
      await sleep(1_000)
    }
  }
  throw new Error('Console API did not become ready')
}

await waitForConsole()

const environmentList = await request('/api/environments')
let environment = environmentList.items.find((item) => item.code === 'demo')
if (!environment) {
  environment = await request('/api/environments', {
    method: 'POST',
    body: JSON.stringify({
      code: 'demo',
      name: 'Local Docker Demo',
      tier: 'local',
      nacosEndpoint,
      nacosNamespace: 'public',
      zone: 'local-demo',
    }),
  })
}

const projectList = await request(`/api/environments/${environment.id}/projects`)
let project = projectList.items.find((item) => item.name === 'demo')
if (!project) {
  const created = await request(`/api/environments/${environment.id}/projects`, {
    method: 'POST',
    body: JSON.stringify({ name: 'demo' }),
  })
  project = await request(`/api/projects/${created.id}`)
}

let draft
try {
  draft = await request(`/api/projects/${project.id}/drafts`)
} catch {
  draft = await request(`/api/projects/${project.id}/drafts`, { method: 'POST' })
}

if (draft.currentRevision === 0) {
  await request(`/api/drafts/${draft.id}/revisions`, {
    method: 'POST',
    headers: { 'If-Match': `"${draft.lockVersion}"` },
    body: JSON.stringify({
      changeSummary: 'Initialize the Docker demo route',
      model: {
        schemaVersion: 1,
        kind: 'project_route',
        hosts: [{ pattern: 'demo.local' }, { pattern: 'localhost' }],
        routes: [
          { path: '/', type: 'RESPONSE', status: 200 },
          { path: '/health', type: 'RESPONSE', status: 200 },
        ],
      },
    }),
  })
}

console.log(`Console demo data is ready (environment=${environment.id}, project=${project.id})`)
