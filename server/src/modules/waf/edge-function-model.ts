// Handwritten edge-function draft model (docs/edge-functions-and-waf-design.md
// §3.1 作者格式, §7 P3 手写函数开放).
//
// Console-internal, function-first authoring model — the same view as the
// wire function index minus `version`/`sha256`, which the release pipeline
// derives. It subsumes the interim mount-first authoring file
// (temp/edge-functions.json): each function carries one project selector whose
// glob semantics match the native codec exactly, and `replaces` turns it into a
// version canary of another function. WAF-generated functions are not part of
// this draft; releases merge them in, so their slugs are reserved here
// (isWafFunctionName).
//
// Hard constraints (§7 "先扛住硬约束") are enforced in stages: these
// field-level caps mirror the native edge-function limits
// (AccessConfigLimits.edgeFunctions), and the native validator compile of
// every script plus the index codec remain the fail-closed authority at
// compile-preview and release time. The VM has no step budget; the shared
// restricted dialect (no function definitions, no while, bounded for-of) is
// what handwritten scripts compile through, so authoring opens no new
// execution capability.

export const EDGE_FUNCTIONS_DRAFT_SCHEMA_VERSION = 1

// Mirrors AccessConfigLimits.edgeFunctions defaults; see the header comment.
export const MAX_DRAFT_FUNCTIONS = 64
export const MAX_GLOBS_PER_SELECTOR = 256
export const MAX_GLOB_BYTES = 255
export const MAX_FUNCTION_NAME_BYTES = 64
export const MAX_DESCRIPTION_BYTES = 255
export const MAX_SCRIPT_BYTES_PER_FUNCTION = 1_048_576
// Transport bound: the whole draft must fit one save request below the API
// body limit (5 MiB); the native snapshot budget is larger (8 MiB).
export const MAX_TOTAL_DRAFT_SCRIPT_BYTES = 4_194_304

export const EDGE_FUNCTION_PRIORITY_MIN = 0
export const EDGE_FUNCTION_PRIORITY_MAX = 1_000_000
export const EDGE_FUNCTION_RATIO_MIN = 0
export const EDGE_FUNCTION_RATIO_MAX = 10_000

/**
 * Execution phase (wire "phase"; omitted = request_head). A request_body
 * function runs after the route matched, only for PROXY routes opted in via
 * the route-level edge_function_body flag, and may reference $req.body;
 * request_head scripts that reference $req.body fail the native compile.
 */
export const EDGE_FUNCTION_PHASES = ['request_head', 'request_body'] as const
export type EdgeFunctionPhase = (typeof EDGE_FUNCTION_PHASES)[number]

export interface EdgeFunctionDraftFunction {
  /** Slug identity ([a-z0-9-], alphanumeric edges, ≤ 64 bytes). */
  readonly name: string
  /** Console-only display note; never reaches the wire. */
  readonly description?: string
  /** Disabled functions stay in the draft but never reach the wire. */
  readonly enabled: boolean
  /** Execution phase; omitted = request_head (the wire default). */
  readonly phase?: EdgeFunctionPhase
  readonly script: string
  /** Project name globs: a project must match include and no exclude. */
  readonly projects: {
    readonly include: readonly string[]
    readonly exclude?: readonly string[]
  }
  /** Execution order hint, ascending; wire default 0. */
  readonly priority?: number
  /** Sticky sampling ratio in ten-thousandths; wire default 10000 = always. */
  readonly ratio?: number
  /**
   * Version canary: the function this one takes over inside `ratio`. Where both
   * are mounted a request runs exactly one of them; the target's own entry
   * stays untouched, so removing this field restores it completely.
   */
  readonly replaces?: string
}

export interface EdgeFunctionsDraft {
  readonly schemaVersion: typeof EDGE_FUNCTIONS_DRAFT_SCHEMA_VERSION
  readonly functions: readonly EdgeFunctionDraftFunction[]
}

export function emptyEdgeFunctionsDraft(): EdgeFunctionsDraft {
  return { schemaVersion: EDGE_FUNCTIONS_DRAFT_SCHEMA_VERSION, functions: [] }
}
