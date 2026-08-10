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
let environment
if (environmentList.items.length === 0) {
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
} else {
  environment = await request('/api/workspace')
}

const projectList = await request('/api/projects')
let project = projectList.items.find((item) => item.domain === 'demo.local')
if (!project) {
  const created = await request('/api/projects', {
    method: 'POST',
    body: JSON.stringify({ domain: 'demo.local' }),
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
        schemaVersion: 2,
        kind: 'project_routes_yaml',
        routes: [
          {
            id: '00000000-0000-4000-8000-000000000001',
            source: `path: /
type: RESPONSE
status: 200
body:
  type: TEXT
  content: Access Gateway Docker demo is running.
`,
          },
          {
            id: '00000000-0000-4000-8000-000000000002',
            source: `path: /health
type: RESPONSE
status: 200
body:
  type: TEXT
  content: ok
`,
          },
        ],
      },
    }),
  })
}

console.log(`Console demo data is ready (project=${project.id})`)
