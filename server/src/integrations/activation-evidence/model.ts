import type { ActivationTargetConfig } from '../../config/env.js'

export type ActivationCandidateStatus =
  'awaiting' | 'processing' | 'ready_to_publish' | 'accepted' | 'rejected'
export type ActivationWatcherState =
  'created' | 'running' | 'failed' | 'stopping' | 'stopped' | 'disabled'
export type ActivationReadinessState =
  'waiting_for_project_list' | 'synchronizing_projects' | 'ready' | 'unavailable' | 'stopped'
export type ActivationNacosLifecycleState =
  'created' | 'starting' | 'running' | 'failed' | 'stopping' | 'stopped'

export interface ActivationEvidenceFailure {
  stage: string
  code: string
  field: string
  offset: number
  observedAtUnixMillis: number
}

export interface ActivationResourceEvidence {
  dataId: string
  group: string
  candidateStatus: ActivationCandidateStatus
  observedMd5: string | null
  activeMd5: string | null
  observedAtUnixMillis: number
  activeAtUnixMillis: number
  failure: ActivationEvidenceFailure | null
}

export interface ActivationProjectEvidence {
  name: string
  dataId: string
  group: string
  subscriptionState: 'subscribing' | 'subscribed' | 'retrying' | 'failed' | 'retiring'
  candidateStatus: ActivationCandidateStatus
  observedMd5: string | null
  observedVersion: number | null
  activeMd5: string | null
  activeVersion: number | null
  activeSnapshotGeneration: string | null
  activeLoaded: boolean
  observedAtUnixMillis: number
  activeAtUnixMillis: number
  failure: ActivationEvidenceFailure | null
}

export interface ActivationEvidence {
  contractVersion: 1
  evidenceRevision: string
  instance: {
    id: string
    buildVersion: string
    buildRevision: string
    startedAtUnixMillis: number
  }
  runtime: { state: 'running' }
  routeSnapshot: {
    generation: string
    fingerprintSha256: string
    publishedAtUnixMillis: number
    publicationMode: 'atomic_request_pin'
  }
  accessConfig: {
    watcherState: ActivationWatcherState
    readinessState: ActivationReadinessState
    projectList: ActivationResourceEvidence
  }
  projects: readonly ActivationProjectEvidence[]
  gray: {
    watcherState: ActivationWatcherState
    resource: ActivationResourceEvidence
    generation: string
    ruleCount: number
  }
  tls: {
    enabled: boolean
    watcherState: ActivationWatcherState
    resource: ActivationResourceEvidence
    version: string
    certificateCount: number
  }
  discovery: {
    clientState: ActivationNacosLifecycleState
    configServiceState: ActivationNacosLifecycleState
    namingServiceState: ActivationNacosLifecycleState
    readyServices: number
    selectableEndpoints: number
    logicalClusters: number
    selectorLeases: number
  }
}

export interface ActivationEvidenceClient {
  collect(target: ActivationTargetConfig): Promise<ActivationEvidence>
}

export class ActivationEvidenceClientError extends Error {
  readonly code: string
  readonly retryableSnapshotChange: boolean

  constructor(code: string, message: string, retryableSnapshotChange = false) {
    super(message)
    this.name = 'ActivationEvidenceClientError'
    this.code = code
    this.retryableSnapshotChange = retryableSnapshotChange
  }
}
