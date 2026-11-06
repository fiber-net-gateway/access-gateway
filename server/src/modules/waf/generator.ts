// WAF policy → edge-function compilation (docs/edge-functions-and-waf-design.md
// §4.2). Pure and deterministic: the same policy always compiles to the same
// script bytes, so the script sha256 (the wire identity) changes only when the
// policy semantics change.
//
// Shape of the generated script:
//   - `cidr` directive declarations at the top (shared, deduplicated across
//     rules by identical CIDR set, named net1..netN in canonical order);
//   - one `if` per rule in priority order (ties keep declaration order),
//     condition parts ordered cheapest-first: exclusions, methods, path
//     prefix/equality chain, CIDR membership, then the authored match
//     expression (always parenthesized);
//   - `action: block` rules gate on the project's effective mode: block mode
//     reports `waf.block` and answers 403 JSON, log mode reports `waf.log`
//     only (no response is sent);
//   - `action: log` rules always report `waf.log`.
//
// Preconditions: the policy passed validateWafPolicy (charset/enum/shape
// guarantees this module relies on, e.g. comment-safe display names and
// round-trippable string escaping).

import { createHash } from 'node:crypto'

import type { WafPolicy, WafProjectMode, WafRule } from './model.js'
import { WAF_FUNCTION_NAME, WAF_MOUNT_PRIORITY } from './model.js'
import { cidrDirectiveLiteral, dialectStringLiteral } from './escaper.js'

export interface WafDirective {
  readonly name: string
  readonly cidrs: readonly string[]
}

export interface WafMountSelector {
  readonly include: readonly string[]
  readonly exclude: readonly string[]
}

export type WafCompilation =
  | { readonly kind: 'unmount'; readonly reason: 'no-rules' | 'no-mounted-projects' }
  | {
      readonly kind: 'mounted'
      readonly script: string
      readonly scriptSha256: string
      readonly functionName: string
      readonly mountPriority: number
      readonly mounts: readonly WafMountSelector[]
      /** Rule ids in generated execution order (priority asc, declaration ties). */
      readonly ruleOrder: readonly string[]
      readonly directives: readonly WafDirective[]
    }

interface ModeSets {
  readonly off: readonly string[]
  readonly log: readonly string[]
  readonly block: readonly string[]
}

function modeSets(policy: WafPolicy): ModeSets {
  const sets: Record<Exclude<WafProjectMode, never>, string[]> = { off: [], log: [], block: [] }
  for (const [project, mode] of Object.entries(policy.projectModes ?? {})) {
    if (mode === 'off' || mode === 'log' || mode === 'block') {
      sets[mode].push(project)
    }
  }
  return {
    off: [...sets.off].sort(),
    log: [...sets.log].sort(),
    block: [...sets.block].sort(),
  }
}

function mountedSelector(policy: WafPolicy, sets: ModeSets): WafMountSelector {
  if (policy.defaultMode === 'off') {
    return { include: [...sets.log, ...sets.block].sort(), exclude: [] }
  }
  return { include: ['*'], exclude: sets.off }
}

// Enumeration predicates for the effective mode. `enumeration === null` means
// "every mounted project" for that side.
function projectInEnumeration(projects: readonly string[]): string | null {
  const [first, ...rest] = projects
  if (first === undefined) {
    return null
  }
  const equality = [`$ctx.project == ${dialectStringLiteral(first)}`]
  for (const project of rest) {
    equality.push(`$ctx.project == ${dialectStringLiteral(project)}`)
  }
  return equality.length === 1 ? (equality[0] ?? '') : `(${equality.join(' || ')})`
}

function projectNotInEnumeration(projects: readonly string[]): string | null {
  if (projects.length === 0) {
    return null
  }
  if (projects.length === 1) {
    const [only] = projects
    return only === undefined ? null : `$ctx.project != ${dialectStringLiteral(only)}`
  }
  return `!${projectInEnumeration(projects)}`
}

function joinOr(terms: readonly string[]): string {
  const [first, ...rest] = terms
  if (first === undefined) {
    return ''
  }
  return rest.length === 0 ? first : `(${[first, ...rest].join(' || ')})`
}

function matchExpression(rule: WafRule): string {
  return rule.match.includes('\n') ? `(\n${rule.match}\n)` : `(${rule.match.trim()})`
}

function ruleCondition(rule: WafRule, directiveByCidrKey: ReadonlyMap<string, string>): string {
  const parts: string[] = []

  for (const exclusion of rule.exclusions ?? []) {
    parts.push(`$ctx.project != ${dialectStringLiteral(exclusion.project)}`)
  }

  const methods = rule.scope?.methods ?? []
  if (methods.length > 0) {
    parts.push(joinOr(methods.map((method) => `$req.method == ${dialectStringLiteral(method)}`)))
  }

  const paths = rule.scope?.paths ?? []
  if (paths.length > 0) {
    parts.push(
      joinOr(
        paths.map((path) =>
          path.endsWith('*')
            ? `strings.hasPrefix($req.path, ${dialectStringLiteral(path.slice(0, -1))})`
            : `$req.path == ${dialectStringLiteral(path)}`,
        ),
      ),
    )
  }

  const cidrs = rule.scope?.sourceCidrs ?? []
  if (cidrs.length > 0) {
    const key = [...cidrs].sort().join(';')
    const directive = directiveByCidrKey.get(key)
    if (directive) {
      parts.push(`${directive}.contains($conn.remote_addr)`)
    }
  }

  parts.push(matchExpression(rule))
  return parts.join(' && ')
}

function reportStatement(rule: WafRule, event: 'waf.block' | 'waf.log'): string {
  return `ctx.report(${dialectStringLiteral(event)}, ${dialectStringLiteral(rule.id)});`
}

function blockStatements(rule: WafRule, indent: string): string[] {
  return [
    `${indent}${reportStatement(rule, 'waf.block')}`,
    `${indent}resp.sendJson(403, {code: ${dialectStringLiteral('WAF_BLOCKED')}, ruleId: ${dialectStringLiteral(rule.id)}});`,
  ]
}

export function compileWafPolicy(policy: WafPolicy): WafCompilation {
  if (policy.rules.length === 0) {
    return { kind: 'unmount', reason: 'no-rules' }
  }

  const sets = modeSets(policy)
  const selector = mountedSelector(policy, sets)
  if (policy.defaultMode === 'off' && selector.include.length === 0) {
    return { kind: 'unmount', reason: 'no-mounted-projects' }
  }

  // Directives: deduplicate by canonical (sorted) CIDR set, then name them in
  // canonical order so author reorderings keep the script hash stable.
  const cidrSets = new Map<string, string[]>()
  for (const rule of policy.rules) {
    const cidrs = rule.scope?.sourceCidrs ?? []
    if (cidrs.length === 0) {
      continue
    }
    const canonical = [...cidrs].sort()
    const key = canonical.join(';')
    if (!cidrSets.has(key)) {
      cidrSets.set(key, canonical)
    }
  }
  const directives: WafDirective[] = [...cidrSets.entries()]
    .sort(([left], [right]) => (left < right ? -1 : left > right ? 1 : 0))
    .map(([, cidrs], index) => ({ name: `net${index + 1}`, cidrs }))
  const directiveByCidrKey = new Map(
    directives.map((directive) => [directive.cidrs.join(';'), directive.name]),
  )

  // Effective-mode reachability over the mounted set (off projects never
  // execute: they are excluded from the mount). With include='*' the full
  // project list is unknown here, so a non-default mode is "possible" — any
  // unlisted project carries the default. The runtime gate expression below
  // decides per request; these flags only choose which branches to emit.
  const blockPossible = policy.defaultMode === 'block' || sets.block.length > 0
  const logPossible = policy.defaultMode === 'log' || sets.log.length > 0

  const lines: string[] = []
  lines.push(
    `// ${WAF_FUNCTION_NAME}: generated by the access-gateway console from the WAF policy; do not edit.`,
  )
  lines.push(
    `// rules=${String(policy.rules.length)} default-mode=${policy.defaultMode} explicit-modes block=${String(sets.block.length)}/log=${String(sets.log.length)}/off=${String(sets.off.length)}`,
  )
  for (const directive of directives) {
    lines.push(`directive ${directive.name} = cidr ${cidrDirectiveLiteral(directive.cidrs)};`)
  }

  // Gate expression that is true exactly when the project's effective mode is
  // block; computed once and reused by every action=block rule. Both branch
  // kinds are reachable here, so the enumerated side is never empty.
  let blockGate: string | null = null
  if (blockPossible && logPossible) {
    blockGate =
      policy.defaultMode === 'block'
        ? projectNotInEnumeration(sets.log)!
        : projectInEnumeration(sets.block)!
  }

  const orderedRules = policy.rules
    .map((rule, index) => ({ rule, index }))
    .sort((left, right) =>
      left.rule.priority !== right.rule.priority
        ? left.rule.priority - right.rule.priority
        : left.index - right.index,
    )

  for (const { rule } of orderedRules) {
    lines.push('')
    lines.push(`// ${rule.id} (priority ${String(rule.priority)}, ${rule.action}): ${rule.name}`)
    const condition = ruleCondition(rule, directiveByCidrKey)
    if (rule.action === 'block' && blockPossible && logPossible && blockGate) {
      lines.push(`if (${condition}) {`)
      lines.push(`    if (${blockGate}) {`)
      lines.push(...blockStatements(rule, '        '))
      lines.push('    } else {')
      lines.push(`        ${reportStatement(rule, 'waf.log')}`)
      lines.push('    }')
      lines.push('}')
    } else if (rule.action === 'block' && blockPossible) {
      lines.push(`if (${condition}) {`)
      lines.push(...blockStatements(rule, '    '))
      lines.push('}')
    } else {
      lines.push(`if (${condition}) {`)
      lines.push(`    ${reportStatement(rule, 'waf.log')}`)
      lines.push('}')
    }
  }

  lines.push('')
  lines.push('return true;')
  lines.push('')

  const script = lines.join('\n')
  const scriptSha256 = createHash('sha256').update(Buffer.from(script, 'utf8')).digest('hex')

  return {
    kind: 'mounted',
    script,
    scriptSha256,
    functionName: WAF_FUNCTION_NAME,
    mountPriority: WAF_MOUNT_PRIORITY,
    mounts: [selector],
    ruleOrder: orderedRules.map(({ rule }) => rule.id),
    directives,
  }
}
