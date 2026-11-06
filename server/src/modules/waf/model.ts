// WAF policy model (docs/edge-functions-and-waf-design.md §4.1).
//
// This is the console-internal authoring model. It never reaches the wire:
// publication compiles it (generator.ts) into one edge-function script plus
// its mount entry, exactly like route YAML compiles into the project-route
// wire model. The data plane has no WAF semantics.

export const WAF_POLICY_SCHEMA_VERSION = 1

// The generated function's slug identity. It flows into the script Data ID,
// metric labels, event logs, and activation evidence; it must satisfy the
// native function-name charset ([a-z0-9-], alphanumeric edges).
export const WAF_FUNCTION_NAME = 'waf'

// Mount priority of the generated function. Matches the design doc's index
// example; only ordering against future non-WAF edge functions matters.
export const WAF_MOUNT_PRIORITY = 100

export type WafSeverity = 'critical' | 'high' | 'medium' | 'low'
export type WafAction = 'log' | 'block'
export type WafProjectMode = 'off' | 'log' | 'block'

// v1 has exactly one phase: request head, before route matching.
export type WafPhase = 'request_head'

export interface WafRuleScope {
  /** Uppercase HTTP method names; omitted or empty = every method. */
  readonly methods?: readonly string[]
  /**
   * Path selector entries: an exact path (`/api/login`) or a prefix pattern
   * (`/api/*`). `*` may only appear as the final character. Omitted or empty
   * = every path.
   */
  readonly paths?: readonly string[]
  /**
   * Source CIDR restriction. The generator hoists these into `cidr`
   * directive declarations (identical sets are deduplicated and shared) and
   * ANDs `<directive>.contains($conn.remote_addr)` into the rule condition.
   * Malformed literals fail the native validator compile: fail-closed.
   */
  readonly sourceCidrs?: readonly string[]
}

export interface WafRuleExclusion {
  /** Project name this rule must not run on. */
  readonly project: string
  /** Audit reason (who excluded what, when, why). Required, never blank. */
  readonly reason: string
}

export interface WafRule {
  /** Stable slug id ([a-z0-9-], alphanumeric edges, ≤ 64 bytes). */
  readonly id: string
  /** Human display name. Console-only metadata. */
  readonly name: string
  readonly severity: WafSeverity
  /** If-chain order: ascending, ties keep declaration order. */
  readonly priority: number
  readonly action: WafAction
  readonly phase: WafPhase
  /**
   * Match expression in the edge-function dialect (same expression language
   * as route conditions). Wrapped in parentheses by the generator; contents
   * are emitted verbatim.
   */
  readonly match: string
  readonly scope?: WafRuleScope
  readonly exclusions?: readonly WafRuleExclusion[]
}

export interface WafPolicy {
  readonly schemaVersion: typeof WAF_POLICY_SCHEMA_VERSION
  /**
   * Mode for projects not listed in `projectModes`. `off` yields a mount
   * with an enumerated include list instead of `*` + excludes.
   */
  readonly defaultMode: WafProjectMode
  readonly rules: readonly WafRule[]
  /** Per-project overrides. Keys are exact project names, not globs. */
  readonly projectModes: Readonly<Record<string, WafProjectMode>>
}

/** The draft a fresh workspace starts from: no rules, everything off. */
export function emptyWafPolicy(): WafPolicy {
  return {
    schemaVersion: WAF_POLICY_SCHEMA_VERSION,
    defaultMode: 'off',
    rules: [],
    projectModes: {},
  }
}
