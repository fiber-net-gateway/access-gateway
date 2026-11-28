import type { ActivationStatus, ActivationSummary } from '../activation/model.js'
import type { ReleaseStatus } from '../releases/state.js'
import type { GrayRuleEntry, GrayRulesDocument } from './model.js'

export type GrayReleaseResourceStatus =
  'pending' | 'running' | 'verified' | 'failed' | 'conflict' | 'conflict_after_partial'

export interface GrayReleaseResourceView {
  id: string
  dataId: string
  group: string
  status: GrayReleaseResourceStatus
  verifiedSha256: string | null
  verifiedAt: string | null
}

export interface GrayReleaseView {
  id: string
  sequence: string
  status: ReleaseStatus
  ruleCount: number
  entries: readonly GrayRuleEntry[]
  wireSha256: string
  description: string
  resource: GrayReleaseResourceView
  publication: {
    jobId: string | null
    state: string | null
  }
  activationStatus: ActivationStatus
  activation: ActivationSummary
  createdAt: string
  publishedAt: string | null
}

export interface CreateGrayReleaseInput {
  idempotencyKey: string
  description?: string
}

export interface RollbackGrayReleaseInput {
  idempotencyKey: string
}

export interface QueueGrayPublicationResult {
  jobId: string
  state: string
  release: GrayReleaseView
}

export interface GrayLiveView {
  exists: boolean
  dataId: string
  group: string
  sha256: string | null
  md5: string | null
  content: string | null
  document: GrayRulesDocument | null
  parseError: string | null
}
