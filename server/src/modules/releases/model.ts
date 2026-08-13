import type { ReleaseStatus } from './state.js'

export type ReleaseResourceStatus =
  'pending' | 'running' | 'verified' | 'failed' | 'conflict' | 'conflict_after_partial'

export interface ReleaseResourceView {
  id: string
  kind: 'project_route' | 'project_list'
  dataId: string
  group: string
  operation: 'upsert' | 'remove'
  status: ReleaseResourceStatus
  targetSha256: string | null
  verifiedSha256: string | null
  verifiedAt: string | null
}

export interface ProjectReleaseView {
  id: string
  sequence: string
  projectId: string
  kind: 'project_route' | 'project_decommission'
  title: string
  description: string
  status: ReleaseStatus
  sourceConfigurationVersion: {
    id: string
    number: number
    relationAtCreation: 'current' | 'historical' | 'unknown'
  } | null
  currentConfigurationVersionAtCreation: {
    id: string
    number: number
  } | null
  allocatedWireVersion: number | null
  sourceModelSha256: string
  wireSha256: string | null
  nativeValidator: {
    contractVersion: number
    revision: string
  } | null
  compilerRevision: string | null
  validationErrors: readonly unknown[]
  resources: readonly ReleaseResourceView[]
  publication: {
    jobId: string | null
    state: string | null
  }
  activationStatus: 'unknown'
  createdAt: string
  publishedAt: string | null
}

export interface ProjectReleaseListResult {
  items: readonly ProjectReleaseView[]
}

export interface CreateProjectReleaseInput {
  sourceVersionId: string
  expectedCurrentVersionId: string
  title: string
  description: string
  idempotencyKey: string
}

export interface CreateProjectDecommissionReleaseInput {
  confirmationDomain: string
  reason: string
  expectedLockVersion: string
  idempotencyKey: string
}

export interface QueuePublicationResult {
  jobId: string
  state: string
  release: ProjectReleaseView
}
