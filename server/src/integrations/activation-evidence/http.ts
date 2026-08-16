import type { ActivationTargetConfig } from '../../config/env.js'
import {
  ActivationEvidenceClientError,
  type ActivationCandidateStatus,
  type ActivationEvidence,
  type ActivationEvidenceClient,
  type ActivationEvidenceFailure,
  type ActivationNacosLifecycleState,
  type ActivationProjectEvidence,
  type ActivationReadinessState,
  type ActivationResourceEvidence,
  type ActivationWatcherState,
} from './model.js'

export interface HttpActivationEvidenceClientOptions {
  timeoutMillis: number
  maxResponseBytes: number
  maxPages: number
  maxProjects: number
}

const candidateStatuses = new Set<ActivationCandidateStatus>([
  'awaiting',
  'processing',
  'ready_to_publish',
  'accepted',
  'rejected',
])
const subscriptionStates = new Set(['subscribing', 'subscribed', 'retrying', 'failed', 'retiring'])
const watcherStates = new Set<ActivationWatcherState>([
  'created',
  'running',
  'failed',
  'stopping',
  'stopped',
  'disabled',
])
const readinessStates = new Set<ActivationReadinessState>([
  'waiting_for_project_list',
  'synchronizing_projects',
  'ready',
  'unavailable',
  'stopped',
])
const nacosLifecycleStates = new Set<ActivationNacosLifecycleState>([
  'created',
  'starting',
  'running',
  'failed',
  'stopping',
  'stopped',
])
const decimalPattern = /^(?:0|[1-9][0-9]*)$/u
const md5Pattern = /^[0-9a-f]{32}$/u
const sha256Pattern = /^[0-9a-f]{64}$/u
const uint64Maximum = 18_446_744_073_709_551_615n
const uint32Maximum = 4_294_967_295
const mysqlDateTimeMaximumUnixMillis = 253_402_300_799_999

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function invalidResponse(): never {
  throw new ActivationEvidenceClientError(
    'ACTIVATION_INVALID_RESPONSE',
    'Activation evidence response is invalid',
  )
}

function boundedText(value: unknown, maximum: number): string {
  if (typeof value !== 'string' || Buffer.byteLength(value, 'utf8') > maximum) invalidResponse()
  return value
}

function enumText<T extends string>(value: unknown, values: ReadonlySet<T>): T {
  if (typeof value !== 'string' || !values.has(value as T)) invalidResponse()
  return value as T
}

function decimal(value: unknown): string {
  const text = boundedText(value, 20)
  if (!decimalPattern.test(text) || BigInt(text) > uint64Maximum) invalidResponse()
  return text
}

function unsignedInteger(value: unknown): number {
  if (!Number.isSafeInteger(value) || (value as number) < 0) invalidResponse()
  return value as number
}

function uint32(value: unknown): number {
  const number = unsignedInteger(value)
  if (number > uint32Maximum) invalidResponse()
  return number
}

function unixMillis(value: unknown): number {
  const number = unsignedInteger(value)
  if (number > mysqlDateTimeMaximumUnixMillis) invalidResponse()
  return number
}

function nullableMd5(value: unknown): string | null {
  if (value === null) return null
  const text = boundedText(value, 32)
  if (!md5Pattern.test(text)) invalidResponse()
  return text
}

function failure(value: unknown): ActivationEvidenceFailure | null {
  if (value === null) return null
  if (!isRecord(value)) invalidResponse()
  return {
    stage: boundedText(value.stage, 32),
    code: boundedText(value.code, 64),
    field: boundedText(value.field, 1_024),
    offset: unsignedInteger(value.offset),
    observedAtUnixMillis: unixMillis(value.observedAtUnixMillis),
  }
}

function resource(value: unknown): ActivationResourceEvidence {
  if (!isRecord(value)) invalidResponse()
  const candidateStatus = value.candidateStatus
  if (
    typeof candidateStatus !== 'string' ||
    !candidateStatuses.has(candidateStatus as ActivationCandidateStatus)
  ) {
    invalidResponse()
  }
  return {
    dataId: boundedText(value.dataId, 512),
    group: boundedText(value.group, 255),
    candidateStatus: candidateStatus as ActivationCandidateStatus,
    observedMd5: nullableMd5(value.observedMd5),
    activeMd5: nullableMd5(value.activeMd5),
    observedAtUnixMillis: unixMillis(value.observedAtUnixMillis),
    activeAtUnixMillis: unixMillis(value.activeAtUnixMillis),
    failure: failure(value.failure),
  }
}

function nullableVersion(value: unknown): number | null {
  if (value === null) return null
  if (
    !Number.isSafeInteger(value) ||
    (value as number) < -2_147_483_648 ||
    (value as number) > 2_147_483_647
  ) {
    invalidResponse()
  }
  return value as number
}

function project(value: unknown): ActivationProjectEvidence {
  if (!isRecord(value)) invalidResponse()
  const candidateStatus = value.candidateStatus
  const subscriptionState = value.subscriptionState
  if (
    typeof candidateStatus !== 'string' ||
    !candidateStatuses.has(candidateStatus as ActivationCandidateStatus) ||
    typeof subscriptionState !== 'string' ||
    !subscriptionStates.has(subscriptionState)
  ) {
    invalidResponse()
  }
  const activeSnapshotGeneration =
    value.activeSnapshotGeneration === null ? null : decimal(value.activeSnapshotGeneration)
  if (typeof value.activeLoaded !== 'boolean') invalidResponse()
  return {
    name: boundedText(value.name, 255),
    dataId: boundedText(value.dataId, 512),
    group: boundedText(value.group, 255),
    subscriptionState: subscriptionState as ActivationProjectEvidence['subscriptionState'],
    candidateStatus: candidateStatus as ActivationCandidateStatus,
    observedMd5: nullableMd5(value.observedMd5),
    observedVersion: nullableVersion(value.observedVersion),
    activeMd5: nullableMd5(value.activeMd5),
    activeVersion: nullableVersion(value.activeVersion),
    activeSnapshotGeneration,
    activeLoaded: value.activeLoaded,
    observedAtUnixMillis: unixMillis(value.observedAtUnixMillis),
    activeAtUnixMillis: unixMillis(value.activeAtUnixMillis),
    failure: failure(value.failure),
  }
}

interface ParsedPage extends Omit<ActivationEvidence, 'projects'> {
  items: readonly ActivationProjectEvidence[]
  nextCursor: string | null
}

function parsePage(value: unknown): ParsedPage {
  if (!isRecord(value) || value.contractVersion !== 1) invalidResponse()
  const instance = value.instance
  const runtime = value.runtime
  const routeSnapshot = value.routeSnapshot
  const accessConfig = value.accessConfig
  const projects = value.projects
  const gray = value.gray
  const tls = value.tls
  const discovery = value.discovery
  if (
    !isRecord(instance) ||
    !isRecord(runtime) ||
    !isRecord(routeSnapshot) ||
    !isRecord(accessConfig) ||
    !isRecord(projects) ||
    !isRecord(gray) ||
    !isRecord(tls) ||
    !isRecord(discovery) ||
    runtime.state !== 'running' ||
    routeSnapshot.publicationMode !== 'atomic_request_pin' ||
    !Array.isArray(projects.items) ||
    projects.items.length > 256 ||
    typeof tls.enabled !== 'boolean'
  ) {
    invalidResponse()
  }
  const fingerprintSha256 = boundedText(routeSnapshot.fingerprintSha256, 64)
  if (!sha256Pattern.test(fingerprintSha256)) invalidResponse()
  const nextCursor = projects.nextCursor
  if (
    nextCursor !== null &&
    (typeof nextCursor !== 'string' ||
      nextCursor.length > 64 ||
      !/^[1-9][0-9]*:[1-9][0-9]*$/u.test(nextCursor))
  ) {
    invalidResponse()
  }
  const ruleCount = uint32(gray.ruleCount)
  const certificateCount = uint32(tls.certificateCount)
  const clientState = enumText(discovery.clientState, nacosLifecycleStates)
  const configServiceState = enumText(discovery.configServiceState, nacosLifecycleStates)
  const namingServiceState = enumText(discovery.namingServiceState, nacosLifecycleStates)
  return {
    contractVersion: 1,
    evidenceRevision: decimal(value.evidenceRevision),
    instance: {
      id: boundedText(instance.id, 255),
      buildVersion: boundedText(instance.buildVersion, 128),
      buildRevision: boundedText(instance.buildRevision, 80),
      startedAtUnixMillis: unixMillis(instance.startedAtUnixMillis),
    },
    runtime: { state: 'running' },
    routeSnapshot: {
      generation: decimal(routeSnapshot.generation),
      fingerprintSha256,
      publishedAtUnixMillis: unixMillis(routeSnapshot.publishedAtUnixMillis),
      publicationMode: 'atomic_request_pin',
    },
    accessConfig: {
      watcherState: enumText(accessConfig.watcherState, watcherStates),
      readinessState: enumText(accessConfig.readinessState, readinessStates),
      projectList: resource(accessConfig.projectList),
    },
    items: projects.items.map(project),
    nextCursor,
    gray: {
      watcherState: enumText(gray.watcherState, watcherStates),
      resource: resource(gray.resource),
      generation: decimal(gray.generation),
      ruleCount,
    },
    tls: {
      enabled: tls.enabled,
      watcherState: enumText(tls.watcherState, watcherStates),
      resource: resource(tls.resource),
      version: decimal(tls.version),
      certificateCount,
    },
    discovery: {
      clientState,
      configServiceState,
      namingServiceState,
      readyServices: uint32(discovery.readyServices),
      selectableEndpoints: uint32(discovery.selectableEndpoints),
      logicalClusters: uint32(discovery.logicalClusters),
      selectorLeases: uint32(discovery.selectorLeases),
    },
  }
}

async function boundedJson(response: Response, maximumBytes: number): Promise<unknown> {
  if (!response.body) {
    throw new ActivationEvidenceClientError(
      'ACTIVATION_EMPTY_RESPONSE',
      'Activation evidence response body is missing',
    )
  }
  const reader = response.body.getReader()
  const chunks: Uint8Array[] = []
  let length = 0
  try {
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      length += value.byteLength
      if (length > maximumBytes) {
        await reader.cancel()
        throw new ActivationEvidenceClientError(
          'ACTIVATION_RESPONSE_LIMIT',
          'Activation evidence response exceeded the byte limit',
        )
      }
      chunks.push(value)
    }
  } finally {
    reader.releaseLock()
  }
  try {
    return JSON.parse(Buffer.concat(chunks, length).toString('utf8')) as unknown
  } catch {
    throw new ActivationEvidenceClientError(
      'ACTIVATION_INVALID_JSON',
      'Activation evidence response is not valid JSON',
    )
  }
}

function sameSnapshot(first: ParsedPage, next: ParsedPage): boolean {
  return (
    first.evidenceRevision === next.evidenceRevision &&
    first.instance.id === next.instance.id &&
    first.instance.startedAtUnixMillis === next.instance.startedAtUnixMillis &&
    first.routeSnapshot.generation === next.routeSnapshot.generation &&
    first.routeSnapshot.fingerprintSha256 === next.routeSnapshot.fingerprintSha256
  )
}

export class HttpActivationEvidenceClient implements ActivationEvidenceClient {
  readonly #options: HttpActivationEvidenceClientOptions

  constructor(options: HttpActivationEvidenceClientOptions) {
    this.#options = options
  }

  async collect(target: ActivationTargetConfig): Promise<ActivationEvidence> {
    const deadline = performance.now() + this.#options.timeoutMillis
    for (let attempt = 0; attempt < 2; attempt += 1) {
      try {
        return await this.collectSnapshot(target, deadline)
      } catch (error) {
        if (!(error instanceof ActivationEvidenceClientError) || !error.retryableSnapshotChange) {
          throw error
        }
      }
    }
    throw new ActivationEvidenceClientError(
      'ACTIVATION_EVIDENCE_CHANGED',
      'Activation evidence changed during pagination',
    )
  }

  private async collectSnapshot(
    target: ActivationTargetConfig,
    deadline: number,
  ): Promise<ActivationEvidence> {
    let cursor: string | null = null
    let first: ParsedPage | null = null
    const projects: ActivationProjectEvidence[] = []
    const projectNames = new Set<string>()
    for (let pageNumber = 0; pageNumber < this.#options.maxPages; pageNumber += 1) {
      const url = new URL(target.endpoint)
      url.searchParams.set('limit', '256')
      if (cursor) url.searchParams.set('cursor', cursor)
      let response: Response
      try {
        const remainingMillis = Math.ceil(deadline - performance.now())
        if (remainingMillis <= 0) {
          throw new ActivationEvidenceClientError(
            'ACTIVATION_REQUEST_TIMEOUT',
            'Activation evidence request timed out',
          )
        }
        response = await fetch(url, {
          headers: {
            Accept: 'application/json',
            Authorization: `Bearer ${target.token}`,
          },
          signal: AbortSignal.timeout(remainingMillis),
          redirect: 'error',
        })
      } catch (error) {
        if (error instanceof ActivationEvidenceClientError) throw error
        throw new ActivationEvidenceClientError(
          'ACTIVATION_TRANSPORT_UNAVAILABLE',
          'Activation evidence request failed',
        )
      }
      if (response.status === 409) {
        await response.body?.cancel()
        throw new ActivationEvidenceClientError(
          'ACTIVATION_EVIDENCE_CHANGED',
          'Activation evidence changed during pagination',
          true,
        )
      }
      if (response.status === 401 || response.status === 403) {
        await response.body?.cancel()
        throw new ActivationEvidenceClientError(
          'ACTIVATION_AUTHENTICATION_FAILED',
          'Activation evidence authentication failed',
        )
      }
      if (!response.ok) {
        await response.body?.cancel()
        throw new ActivationEvidenceClientError(
          'ACTIVATION_HTTP_FAILED',
          'Access-server rejected the activation evidence request',
        )
      }
      const contentType = response.headers.get('content-type')?.toLowerCase() ?? ''
      if (!contentType.startsWith('application/json')) {
        await response.body?.cancel()
        throw new ActivationEvidenceClientError(
          'ACTIVATION_CONTENT_TYPE_INVALID',
          'Activation evidence response content type is invalid',
        )
      }
      const page = parsePage(await boundedJson(response, this.#options.maxResponseBytes))
      if (page.instance.id !== target.instanceKey) {
        throw new ActivationEvidenceClientError(
          'ACTIVATION_INSTANCE_MISMATCH',
          'Activation evidence instance identity did not match the configured target',
        )
      }
      if (first && !sameSnapshot(first, page)) {
        throw new ActivationEvidenceClientError(
          'ACTIVATION_EVIDENCE_CHANGED',
          'Activation evidence changed during pagination',
          true,
        )
      }
      first ??= page
      for (const projectEvidence of page.items) {
        if (projectNames.has(projectEvidence.name)) invalidResponse()
        projectNames.add(projectEvidence.name)
        projects.push(projectEvidence)
      }
      if (projects.length > this.#options.maxProjects) {
        throw new ActivationEvidenceClientError(
          'ACTIVATION_PROJECT_LIMIT',
          'Activation evidence exceeded the Project limit',
        )
      }
      cursor = page.nextCursor
      if (cursor) {
        const [cursorRevision, cursorOffset] = cursor.split(':')
        if (cursorRevision !== page.evidenceRevision || Number(cursorOffset) !== projects.length) {
          invalidResponse()
        }
      }
      if (!cursor) {
        const { items: _items, nextCursor: _nextCursor, ...snapshot } = first
        return { ...snapshot, projects }
      }
    }
    throw new ActivationEvidenceClientError(
      'ACTIVATION_PAGE_LIMIT',
      'Activation evidence exceeded the page limit',
    )
  }
}
