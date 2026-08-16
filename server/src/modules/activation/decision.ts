import type {
  ActivationEvidence,
  ActivationProjectEvidence,
  ActivationResourceEvidence,
} from '../../integrations/activation-evidence/model.js'
import type { ActivationStatus } from './model.js'

export interface ActivationReleaseResource {
  kind: 'project_route' | 'project_list' | 'tls_certificates'
  dataId: string
  group: string
  operation: 'upsert' | 'remove'
  verifiedNacosMd5: string | null
  allocatedProjectVersion: number | null
  projectName: string | null
}

export interface ActivationDecisionInput {
  pollErrorCode: string | null
  evidence: ActivationEvidence | null
  resources: readonly ActivationReleaseResource[]
}

function explicitlyRejected(targetMd5: string, candidate: ActivationResourceEvidence): boolean {
  return (
    candidate.observedMd5 === targetMd5 &&
    (candidate.candidateStatus === 'rejected' || candidate.failure !== null)
  )
}

function projectExplicitlyRejected(
  targetMd5: string,
  targetVersion: number | null,
  candidate: ActivationProjectEvidence,
): boolean {
  return (
    candidate.observedMd5 === targetMd5 &&
    (targetVersion === null || candidate.observedVersion === targetVersion) &&
    (candidate.candidateStatus === 'rejected' || candidate.failure !== null)
  )
}

function sameResourceIdentity(
  resource: ActivationReleaseResource,
  candidate: ActivationResourceEvidence | ActivationProjectEvidence,
): boolean {
  return candidate.dataId === resource.dataId && candidate.group === resource.group
}

export function decideInstanceActivation(input: ActivationDecisionInput): ActivationStatus {
  if (input.pollErrorCode || !input.evidence) return 'degraded'
  if (input.resources.length === 0) return 'degraded'

  const routeWatcherFailed =
    input.evidence.accessConfig.watcherState === 'failed' ||
    input.evidence.accessConfig.readinessState === 'unavailable' ||
    input.evidence.accessConfig.readinessState === 'stopped'
  const tlsWatcherFailed =
    !input.evidence.tls.enabled ||
    input.evidence.tls.watcherState === 'failed' ||
    input.evidence.tls.watcherState === 'stopped'
  let exactResources = 0
  let rejectedTarget = false
  for (const resource of input.resources) {
    const targetMd5 = resource.verifiedNacosMd5
    if (resource.operation === 'remove') {
      if (resource.kind !== 'project_route' || !resource.projectName) {
        rejectedTarget = true
        continue
      }
      if (!input.evidence.projects.some((project) => project.name === resource.projectName)) {
        exactResources += 1
      }
      continue
    }
    if (!targetMd5) {
      rejectedTarget = true
      continue
    }

    if (resource.kind === 'project_list') {
      const candidate = input.evidence.accessConfig.projectList
      if (!sameResourceIdentity(resource, candidate)) rejectedTarget = true
      else if (candidate.activeMd5 === targetMd5) exactResources += 1
      else rejectedTarget ||= routeWatcherFailed || explicitlyRejected(targetMd5, candidate)
      continue
    }

    if (resource.kind === 'tls_certificates') {
      const candidate = input.evidence.tls.resource
      const versionMatches =
        resource.allocatedProjectVersion === null ||
        input.evidence.tls.version === String(resource.allocatedProjectVersion)
      if (!sameResourceIdentity(resource, candidate)) rejectedTarget = true
      else if (candidate.activeMd5 === targetMd5 && versionMatches) exactResources += 1
      else rejectedTarget ||= tlsWatcherFailed || explicitlyRejected(targetMd5, candidate)
      continue
    }

    const candidate = input.evidence.projects.find(
      (project) => project.name === resource.projectName,
    )
    if (!candidate) {
      rejectedTarget ||= routeWatcherFailed
      continue
    }
    if (!sameResourceIdentity(resource, candidate)) {
      rejectedTarget = true
      continue
    }
    const versionMatches =
      resource.allocatedProjectVersion === null ||
      candidate.activeVersion === resource.allocatedProjectVersion
    if (candidate.activeLoaded && candidate.activeMd5 === targetMd5 && versionMatches) {
      exactResources += 1
    } else {
      rejectedTarget ||=
        routeWatcherFailed ||
        candidate.subscriptionState === 'failed' ||
        projectExplicitlyRejected(targetMd5, resource.allocatedProjectVersion, candidate)
    }
  }

  if (exactResources === input.resources.length) return 'active'
  return rejectedTarget ? 'degraded' : 'pending'
}
