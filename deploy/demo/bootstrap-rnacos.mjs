import { readFile } from 'node:fs/promises'

const rnacosUrl = process.env.RNACOS_URL ?? 'http://rnacos:8848'
const route = await readFile('/demo/demo-route.json', 'utf8')
const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds))

async function waitForRnacos() {
  const readinessUrl = new URL('/nacos/v1/cs/configs', rnacosUrl)
  readinessUrl.search = new URLSearchParams({
    dataId: 'access-gateway-readiness',
    group: 'DEFAULT_GROUP',
  })

  for (let attempt = 0; attempt < 60; attempt += 1) {
    try {
      await fetch(readinessUrl, { signal: AbortSignal.timeout(2_000) })
      return
    } catch {
      await sleep(1_000)
    }
  }
  throw new Error('R-Nacos did not become reachable')
}

async function publish(dataId, group, content, type = 'text') {
  const response = await fetch(new URL('/nacos/v1/cs/configs', rnacosUrl), {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ dataId, group, content, type }),
  })
  const result = await response.text()
  if (!response.ok || result !== 'true') {
    throw new Error(`R-Nacos rejected demo config ${group}/${dataId}`)
  }
}

async function readConfig(dataId, group) {
  const url = new URL('/nacos/v1/cs/configs', rnacosUrl)
  url.search = new URLSearchParams({ dataId, group })
  const response = await fetch(url)
  if (!response.ok) {
    throw new Error(`R-Nacos readback failed for ${group}/${dataId}`)
  }
  return response.text()
}

await waitForRnacos()
await publish('ploto.unified-access.projects', 'ACCESS-SERVER', 'demo')
await publish('ploto.unified-access.route.demo', 'ACCESS-SERVER', route, 'json')

if ((await readConfig('ploto.unified-access.projects', 'ACCESS-SERVER')) !== 'demo') {
  throw new Error('R-Nacos project-list readback did not match')
}
await readConfig('ploto.unified-access.route.demo', 'ACCESS-SERVER')

console.log('R-Nacos demo configuration is ready')
