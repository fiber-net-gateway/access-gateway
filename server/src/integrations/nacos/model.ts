export interface NacosTarget {
  endpoint: string
  namespace: string
  tenant: string
  credentialConfigured: boolean
}

export interface NacosResourceValue {
  exists: boolean
  content: string | null
  sha256: string | null
  md5: string | null
}

export interface NacosClient {
  readonly available: boolean
  read(target: NacosTarget, dataId: string, group: string): Promise<NacosResourceValue>
  write(
    target: NacosTarget,
    dataId: string,
    group: string,
    content: string,
    type: 'text' | 'json',
  ): Promise<void>
}
