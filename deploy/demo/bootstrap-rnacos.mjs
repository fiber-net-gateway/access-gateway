const rnacosUrl = process.env.RNACOS_URL ?? 'http://rnacos:8848'
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

await waitForRnacos()
console.log('R-Nacos is ready for Console publication')
