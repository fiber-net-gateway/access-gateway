import type { ActivationStatus, ActivationSummary } from '../activation/model.js'
import type { ReleaseStatus } from '../releases/state.js'

export interface TlsCertificateReleaseView {
  id: string
  sequence: string
  status: ReleaseStatus
  defaultCertificateId: string
  certificateCount: number
  wireSha256: string
  resource: {
    id: string
    dataId: string
    group: string
    status: 'pending' | 'running' | 'verified' | 'failed' | 'conflict' | 'conflict_after_partial'
    verifiedSha256: string | null
    verifiedAt: string | null
  }
  publication: {
    jobId: string | null
    state: string | null
  }
  activationStatus: ActivationStatus
  activation: ActivationSummary
  createdAt: string
  publishedAt: string | null
}

export interface CreateTlsCertificateReleaseInput {
  defaultCertificateId: string
  idempotencyKey: string
}

export interface QueueTlsCertificatePublicationResult {
  jobId: string
  state: string
  release: TlsCertificateReleaseView
}
