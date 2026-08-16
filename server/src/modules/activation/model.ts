import type { Actor } from '../auth/model.js'

export const activationStatuses = ['unknown', 'pending', 'active', 'degraded'] as const

export type ActivationStatus = (typeof activationStatuses)[number]

export interface ActivationSummary {
  status: ActivationStatus
  targetCount: number
  activeCount: number
  pendingCount: number
  degradedCount: number
  unknownCount: number
  evaluatedAt: string | null
}

export interface ActivationInstanceView {
  id: string
  instanceKey: string
  status: ActivationStatus
  buildVersion: string | null
  buildRevision: string | null
  evidenceRevision: string | null
  routeSnapshotGeneration: string | null
  routeSnapshotFingerprintSha256: string | null
  candidateStatus: string | null
  candidateErrorCode: string | null
  activeMd5: string | null
  activeVersion: string | null
  observedAt: string | null
  expiresAt: string | null
}

export interface ActivationInstanceList {
  releaseId: string
  summary: ActivationSummary
  items: readonly ActivationInstanceView[]
  nextCursor: string | null
}

export interface ActivationService {
  listReleaseInstances(
    actor: Actor,
    releaseId: string,
    cursor: string | null,
    limit: number,
  ): Promise<ActivationInstanceList>
}

export function unknownActivationSummary(): ActivationSummary {
  return {
    status: 'unknown',
    targetCount: 0,
    activeCount: 0,
    pendingCount: 0,
    degradedCount: 0,
    unknownCount: 0,
    evaluatedAt: null,
  }
}
