// WAF release view model + the edge-function rnacos wire constants
// (docs/edge-functions-and-waf-design.md §3.1).

import type { ActivationStatus, ActivationSummary } from '../activation/model.js'
import type { ReleaseStatus } from '../releases/state.js'

export const EDGE_FUNCTIONS_DATA_ID = 'ploto.unified-access.edge-functions'
export const EDGE_FUNCTION_SCRIPT_DATA_ID_PREFIX = 'ploto.unified-access.edge-function.'
export const EDGE_FUNCTIONS_GROUP = 'ACCESS-SERVER'

export function edgeFunctionScriptDataId(functionName: string): string {
  return `${EDGE_FUNCTION_SCRIPT_DATA_ID_PREFIX}${functionName}`
}

export type WafReleaseResourceStatus =
  'pending' | 'running' | 'verified' | 'failed' | 'conflict' | 'conflict_after_partial'

export interface WafReleaseResourceView {
  id: string
  kind: 'edge_function_script' | 'edge_function_index'
  dataId: string
  group: string
  status: WafReleaseResourceStatus
  verifiedSha256: string | null
  verifiedAt: string | null
}

export interface WafReleaseView {
  id: string
  sequence: string
  status: ReleaseStatus
  functionName: string
  /** 'unmount' releases publish an empty function index. */
  mode: 'mounted' | 'unmount'
  scriptSha256: string | null
  ruleCount: number
  /** The wire index version embedded in the published index. */
  indexVersion: string
  /** Empty for direct releases; records the source release for rollbacks. */
  description: string
  resources: readonly WafReleaseResourceView[]
  publication: {
    jobId: string | null
    state: string | null
  }
  activationStatus: ActivationStatus
  activation: ActivationSummary
  createdAt: string
  publishedAt: string | null
}

export interface CreateWafReleaseInput {
  idempotencyKey: string
}

export interface RollbackWafReleaseInput {
  idempotencyKey: string
}

export interface QueueWafPublicationResult {
  jobId: string
  state: string
  release: WafReleaseView
}
