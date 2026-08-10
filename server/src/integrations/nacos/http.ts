import { createHash } from 'node:crypto'

import { unavailable } from '../../shared/errors.js'
import type { NacosClient, NacosResourceValue, NacosTarget } from './model.js'

export interface HttpNacosClientOptions {
  timeoutMillis: number
  maxResponseBytes: number
  endpointOverride?: string | null
}

function digest(algorithm: 'md5' | 'sha256', content: string): string {
  return createHash(algorithm).update(content, 'utf8').digest('hex')
}

function configUrl(target: NacosTarget, dataId: string, group: string): URL {
  const url = new URL('/nacos/v1/cs/configs', target.endpoint)
  url.search = new URLSearchParams({
    dataId,
    group,
    ...(target.tenant ? { tenant: target.tenant } : {}),
  }).toString()
  return url
}

function requireSupportedCredentials(target: NacosTarget): void {
  if (target.credentialConfigured) {
    throw unavailable(
      'NACOS_CREDENTIAL_PROVIDER_UNAVAILABLE',
      'The configured Nacos credential reference cannot be resolved by this deployment',
    )
  }
}

export class HttpNacosClient implements NacosClient {
  readonly available = true
  readonly #timeoutMillis: number
  readonly #maxResponseBytes: number
  readonly #endpointOverride: string | null

  constructor(options: HttpNacosClientOptions) {
    this.#timeoutMillis = options.timeoutMillis
    this.#maxResponseBytes = options.maxResponseBytes
    this.#endpointOverride = options.endpointOverride ?? null
  }

  async read(target: NacosTarget, dataId: string, group: string): Promise<NacosResourceValue> {
    requireSupportedCredentials(target)
    let response: Response
    try {
      response = await fetch(
        configUrl(
          this.#endpointOverride ? { ...target, endpoint: this.#endpointOverride } : target,
          dataId,
          group,
        ),
        {
          headers: { Accept: 'text/plain' },
          signal: AbortSignal.timeout(this.#timeoutMillis),
        },
      )
    } catch {
      throw unavailable('NACOS_READ_UNAVAILABLE', 'Nacos configuration read failed')
    }
    if (response.status === 404) {
      return { exists: false, content: null, sha256: null, md5: null }
    }
    if (!response.ok) {
      throw unavailable('NACOS_READ_FAILED', 'Nacos rejected a configuration read')
    }
    const content = await response.text()
    if (Buffer.byteLength(content, 'utf8') > this.#maxResponseBytes) {
      throw unavailable('NACOS_RESPONSE_LIMIT', 'Nacos configuration exceeded the response limit')
    }
    return {
      exists: true,
      content,
      sha256: digest('sha256', content),
      md5: digest('md5', content),
    }
  }

  async write(
    target: NacosTarget,
    dataId: string,
    group: string,
    content: string,
    type: 'text' | 'json',
  ): Promise<void> {
    requireSupportedCredentials(target)
    let response: Response
    try {
      response = await fetch(
        configUrl(
          this.#endpointOverride ? { ...target, endpoint: this.#endpointOverride } : target,
          dataId,
          group,
        ),
        {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({ dataId, group, content, type }),
          signal: AbortSignal.timeout(this.#timeoutMillis),
        },
      )
    } catch {
      throw unavailable('NACOS_WRITE_UNAVAILABLE', 'Nacos configuration write failed')
    }
    const result = await response.text()
    if (!response.ok || result.trim() !== 'true') {
      throw unavailable('NACOS_WRITE_FAILED', 'Nacos rejected a configuration write')
    }
  }
}

export class UnavailableNacosClient implements NacosClient {
  readonly available = false

  async read(): Promise<never> {
    throw unavailable('NACOS_CLIENT_UNCONFIGURED', 'Nacos publication is not configured')
  }

  async write(): Promise<never> {
    throw unavailable('NACOS_CLIENT_UNCONFIGURED', 'Nacos publication is not configured')
  }
}
