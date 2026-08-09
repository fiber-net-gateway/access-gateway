export interface ResourceRevision {
  exists: boolean
  sha256: string | null
}

export interface PublicationDecisionInput {
  operation: 'upsert' | 'remove'
  before: ResourceRevision
  base: ResourceRevision
  targetSha256: string | null
  previouslyWritten: boolean
}

export type PublicationDecision =
  'write' | 'remove' | 'verify_target' | 'conflict' | 'conflict_after_partial'

function sameRevision(left: ResourceRevision, right: ResourceRevision): boolean {
  return left.exists === right.exists && left.sha256 === right.sha256
}

export function decidePublication(input: PublicationDecisionInput): PublicationDecision {
  const targetMatches =
    input.operation === 'upsert'
      ? input.before.exists && input.before.sha256 === input.targetSha256
      : !input.before.exists
  if (targetMatches) {
    return 'verify_target'
  }
  if (!sameRevision(input.before, input.base)) {
    return input.previouslyWritten ? 'conflict_after_partial' : 'conflict'
  }
  return input.operation === 'upsert' ? 'write' : 'remove'
}

export type RequiredResourceStatus =
  'pending' | 'running' | 'verified' | 'failed' | 'conflict' | 'conflict_after_partial'

export function aggregateReleasePublication(
  statuses: readonly RequiredResourceStatus[],
  changedTargetEnvironment: boolean,
): 'published' | 'partially_published' | 'publish_failed' | 'publishing' {
  if (statuses.length > 0 && statuses.every((status) => status === 'verified')) {
    return 'published'
  }
  if (changedTargetEnvironment) {
    return 'partially_published'
  }
  if (
    statuses.some((status) => ['failed', 'conflict', 'conflict_after_partial'].includes(status))
  ) {
    return 'publish_failed'
  }
  return 'publishing'
}
