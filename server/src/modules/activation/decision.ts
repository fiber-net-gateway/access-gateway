import type {
  ActivationEvidence,
  ActivationProjectEvidence,
  ActivationResourceEvidence,
} from '../../integrations/activation-evidence/model.js'
import type { ActivationStatus } from './model.js'

export interface ActivationReleaseResource {
  kind:
    | 'project_route'
    | 'project_list'
    | 'tls_certificates'
    | 'edge_function_script'
    | 'edge_function_index'
  dataId: string
  group: string
  operation: 'upsert' | 'remove'
  verifiedNacosMd5: string | null
  verifiedSha256: string | null
  allocatedProjectVersion: number | null
  projectName: string | null
}

export interface ActivationDecisionInput {
  pollErrorCode: string | null
  evidence: ActivationEvidence | null
  resources: readonly ActivationReleaseResource[]
}

interface EdgeIndexEvaluation {
  exact: boolean
  rejected: boolean
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

// The edge-function decision matches the live index, not the scripts: the data
// plane flips its active edge snapshot only after every script hash listed in
// the index was validated and compiled (two-phase assembly), and the index
// content itself carries each script's sha256. The live index matching the
// published index bytes (md5 and sha256 from the publication readback) is
// therefore typed per-instance proof that the whole release is active, and it
// covers the script resource transitively. A watcher that keeps the old
// snapshot after rejecting the published bytes stays verifiable as degraded.
function evaluateEdgeIndex(
  resources: readonly ActivationReleaseResource[],
  evidence: NonNullable<ActivationEvidence['edgeFunctions']>,
  watcherFailed: boolean,
): EdgeIndexEvaluation {
  const indexResource = resources.find((resource) => resource.kind === 'edge_function_index')
  if (!indexResource || indexResource.operation !== 'upsert')
    return { exact: false, rejected: false }
  const candidate = evidence.index
  if (!sameResourceIdentity(indexResource, candidate)) return { exact: false, rejected: true }
  const md5Matches =
    indexResource.verifiedNacosMd5 !== null &&
    candidate.activeMd5 === indexResource.verifiedNacosMd5
  const sha256Matches =
    indexResource.verifiedSha256 !== null &&
    evidence.activeIndexSha256 === indexResource.verifiedSha256
  if (md5Matches && sha256Matches) return { exact: true, rejected: false }
  return {
    exact: false,
    rejected:
      watcherFailed ||
      (indexResource.verifiedNacosMd5 !== null &&
        explicitlyRejected(indexResource.verifiedNacosMd5, candidate)),
  }
}

export function decideInstanceActivation(input: ActivationDecisionInput): ActivationStatus {
  if (input.pollErrorCode || !input.evidence) return 'degraded'
  if (input.resources.length === 0) return 'degraded'

  const edgeResources = input.resources.filter(
    (resource) =>
      resource.kind === 'edge_function_script' || resource.kind === 'edge_function_index',
  )
  // Builds without edge-function support never emit the edgeFunctions evidence
  // block; such instances cannot attest an edge-function release, so the
  // release stays unknown instead of being inferred from a nacos readback.
  if (edgeResources.length > 0 && !input.evidence.edgeFunctions) return 'unknown'
  const edgeEvidence = input.evidence.edgeFunctions ?? null
  const edgeWatcherFailed =
    !edgeEvidence ||
    edgeEvidence.watcherState === 'failed' ||
    edgeEvidence.watcherState === 'stopped'
  const edgeIndex = edgeEvidence
    ? evaluateEdgeIndex(input.resources, edgeEvidence, edgeWatcherFailed)
    : { exact: false, rejected: false }

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

    if (resource.kind === 'edge_function_index' || resource.kind === 'edge_function_script') {
      if (edgeIndex.exact) exactResources += 1
      else rejectedTarget ||= edgeIndex.rejected
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
