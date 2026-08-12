import { readFile } from 'node:fs/promises'
import { X509Certificate } from 'node:crypto'

const apiUrl = process.env.CONSOLE_API_URL ?? 'http://console-api:3000'
const nacosEndpoint = process.env.DEMO_NACOS_ENDPOINT ?? 'http://rnacos:8848'

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
    const code = body?.error?.code ?? 'UNKNOWN'
    throw new Error(
      `Console API ${init.method ?? 'GET'} ${path} failed (${response.status}, ${code})`,
    )
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
if (environmentList.items.length === 0) {
  await request('/api/environments', {
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

const [demoCertificatePem, demoPrivateKeyPem] = await Promise.all([
  readFile('/demo/certs/demo.crt', 'utf8'),
  readFile('/demo/certs/demo.key', 'utf8'),
])
let certificateList = await request('/api/certificates')
let demoCertificate =
  certificateList.items.find((item) => item.currentVersion.dnsNames.includes('demo.local')) ??
  certificateList.items.find((item) => item.name === 'Docker demo certificate')
if (!demoCertificate) {
  demoCertificate = await request('/api/certificates', {
    method: 'POST',
    body: JSON.stringify({
      name: 'Docker demo certificate',
      certificatePem: demoCertificatePem,
      privateKeyPem: demoPrivateKeyPem,
    }),
  })
}
const demoCertificateFingerprint = new X509Certificate(demoCertificatePem).fingerprint256
  .replaceAll(':', '')
  .toLowerCase()
if (demoCertificate.currentVersion.fingerprintSha256 !== demoCertificateFingerprint) {
  demoCertificate = await request(`/api/certificates/${demoCertificate.id}/versions`, {
    method: 'POST',
    headers: { 'If-Match': `"${demoCertificate.lockVersion}"` },
    body: JSON.stringify({
      certificatePem: demoCertificatePem,
      privateKeyPem: demoPrivateKeyPem,
      confirmSniCoverageChange: true,
    }),
  })
}

let tlsReleaseList = await request('/api/tls/releases')
let tlsRelease = tlsReleaseList.items.find(
  (item) =>
    ['ready', 'queued', 'publishing', 'published'].includes(item.status) &&
    Date.parse(item.createdAt) >= Date.parse(demoCertificate.updatedAt),
)
if (!tlsRelease) {
  tlsRelease = await request('/api/tls/releases', {
    method: 'POST',
    headers: { 'Idempotency-Key': `docker-demo-tls-v${demoCertificate.currentVersion.version}` },
    body: JSON.stringify({ defaultCertificateId: demoCertificate.id }),
  })
}
if (tlsRelease.status === 'ready') {
  const queued = await request(`/api/tls/releases/${tlsRelease.id}/publications`, {
    method: 'POST',
    headers: { 'Idempotency-Key': `docker-demo-tls-publish-${tlsRelease.id}` },
  })
  tlsRelease = queued.release
}
for (let attempt = 0; attempt < 120 && tlsRelease.status !== 'published'; attempt += 1) {
  if (['partially_published', 'publish_failed', 'abandoned'].includes(tlsRelease.status)) {
    throw new Error(`Demo TLS Release stopped in ${tlsRelease.status}`)
  }
  await sleep(1_000)
  tlsReleaseList = await request('/api/tls/releases')
  tlsRelease = tlsReleaseList.items.find((item) => item.id === tlsRelease.id)
  if (!tlsRelease) throw new Error('Demo TLS Release disappeared')
}
if (tlsRelease.status !== 'published') {
  throw new Error('Demo TLS Release did not publish within 120 seconds')
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

let versionList = await request(`/api/projects/${project.id}/configuration-versions`)
if (versionList.items.length === 0) {
  const saved = await request(`/api/projects/${project.id}/configuration-versions`, {
    method: 'POST',
    headers: {
      'If-Match': `"${versionList.lockVersion}"`,
      'Idempotency-Key': 'docker-demo-initial-version',
    },
    body: JSON.stringify({
      baseVersionId: null,
      changeSummary: 'Initialize the Docker demo routes',
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
  versionList = await request(`/api/projects/${project.id}/configuration-versions`)
  if (versionList.currentVersionId !== saved.version.id) {
    throw new Error('Saved demo version did not become current')
  }
}

const currentVersion = versionList.items.find(
  (version) => version.id === versionList.currentVersionId,
)
if (!currentVersion) throw new Error('Demo project has no current configuration version')

let releaseList = await request(`/api/projects/${project.id}/releases`)
let release = releaseList.items.find((item) =>
  ['ready', 'queued', 'publishing', 'published'].includes(item.status),
)
if (!release) {
  release = await request(`/api/projects/${project.id}/releases`, {
    method: 'POST',
    headers: { 'Idempotency-Key': `docker-demo-release-v${currentVersion.number}` },
    body: JSON.stringify({
      sourceVersionId: currentVersion.id,
      expectedCurrentVersionId: currentVersion.id,
      title: `Docker demo V${currentVersion.number}`,
      description: 'Publish the deterministic Docker demo routes to rnacos',
    }),
  })
}
if (release.status === 'ready') {
  const queued = await request(`/api/releases/${release.id}/publications`, {
    method: 'POST',
    headers: { 'Idempotency-Key': `docker-demo-publish-${release.id}` },
  })
  release = queued.release
}

for (let attempt = 0; attempt < 120 && release.status !== 'published'; attempt += 1) {
  if (
    ['partially_published', 'publish_failed', 'validation_failed', 'abandoned'].includes(
      release.status,
    )
  ) {
    throw new Error(`Demo Release stopped in ${release.status}`)
  }
  await sleep(1_000)
  release = await request(`/api/releases/${release.id}`)
}
if (release.status !== 'published')
  throw new Error('Demo Release did not publish within 120 seconds')

console.log(
  `Console demo data is ready (project=${project.id}, version=V${currentVersion.number}, release=R${release.sequence}, tls=R${tlsRelease.sequence})`,
)
