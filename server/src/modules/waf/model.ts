// WAF policy model (docs/edge-functions-and-waf-design.md §4.1).
//
// This is the console-internal authoring model. It never reaches the wire:
// publication compiles it (generator.ts) into one edge-function script plus
// mount entry per rule group, exactly like route YAML compiles into the
// project-route wire model. The data plane has no WAF semantics.

export const WAF_POLICY_SCHEMA_VERSION = 2

// A group compiles to the edge function `waf-<group id>`. The name flows into
// the script Data ID, metric labels, event logs, and activation evidence, so
// it must satisfy the native function-name charset ([a-z0-9-], alphanumeric
// edges, ≤ 64 bytes) and a group id can never be renamed in place.
export const WAF_FUNCTION_PREFIX = 'waf-'

// The single function schema-1 policies compiled to. It only survives in
// release history, where a rollback can publish it again, so it stays reserved.
export const LEGACY_WAF_FUNCTION_NAME = 'waf'

export function wafFunctionName(groupId: string): string {
  return `${WAF_FUNCTION_PREFIX}${groupId}`
}

/** True for every slug the WAF generator owns, past or present. */
export function isWafFunctionName(name: string): boolean {
  return name === LEGACY_WAF_FUNCTION_NAME || name.startsWith(WAF_FUNCTION_PREFIX)
}

export type WafSeverity = 'critical' | 'high' | 'medium' | 'low'
export type WafAction = 'log' | 'block'
export type WafMode = 'off' | 'log' | 'block'

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
   * directive declarations (identical sets are deduplicated and shared within
   * a group) and ANDs `<directive>.contains($conn.remote_addr)` into the rule
   * condition. Malformed literals fail the native validator compile:
   * fail-closed.
   */
  readonly sourceCidrs?: readonly string[]
}

export interface WafRule {
  /** Stable slug id ([a-z0-9-], alphanumeric edges, ≤ 64 bytes), policy-wide. */
  readonly id: string
  /** Human display name. Console-only metadata. */
  readonly name: string
  /** Id of the one group this rule belongs to. */
  readonly group: string
  readonly severity: WafSeverity
  /** If-chain order inside the group: ascending, ties keep declaration order. */
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
}

export interface WafGroupExemption {
  /** Project name or glob this group must not run on. */
  readonly project: string
  /** Audit reason (who exempted what, when, why). Required, never blank. */
  readonly reason: string
}

export interface WafGroup {
  /** Stable slug id ([a-z0-9-], alphanumeric edges, ≤ 60 bytes). */
  readonly id: string
  /** Human display name. Console-only metadata. */
  readonly name: string
  /** Mode for projects without an override; `off` publishes no function. */
  readonly mode: WafMode
  /** Mount priority: groups run ascending, ties broken by id. */
  readonly priority: number
  readonly projects: {
    /** Project globs the group applies to; the wire mount `include`. */
    readonly include: readonly string[]
    /** Vetoes `include`; the wire mount `exclude`. */
    readonly exempt?: readonly WafGroupExemption[]
  }
}

export interface WafPolicy {
  readonly schemaVersion: typeof WAF_POLICY_SCHEMA_VERSION
  readonly groups: readonly WafGroup[]
  readonly rules: readonly WafRule[]
  /**
   * Per-project overrides of the group mode, across every group the project
   * belongs to. Keys are exact project names, not globs.
   */
  readonly projectModes: Readonly<Record<string, WafMode>>
}

/** The draft a fresh workspace starts from: nothing to publish. */
export function emptyWafPolicy(): WafPolicy {
  return {
    schemaVersion: WAF_POLICY_SCHEMA_VERSION,
    groups: [],
    rules: [],
    projectModes: {},
  }
}
