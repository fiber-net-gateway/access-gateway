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

// Where one rule landed in the generated script. Validator errors carry script
// byte offsets; authors only know their rules, so previews map offsets back
// through these spans. Console-side metadata: it never reaches the wire.
export interface WafRuleSpan {
  /** Index of the rule in policy.rules (declaration order). */
  readonly ruleIndex: number
  /** Script byte range of the rule's block; `end` is exclusive. */
  readonly start: number
  readonly end: number
  /** Script byte offset of the authored match text inside the block. */
  readonly matchStart: number
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
      readonly ruleSpans: readonly WafRuleSpan[]
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

// The authored text exactly as it is embedded: single-line expressions are
// trimmed, multi-line ones keep their layout on their own lines.
function embeddedMatch(rule: WafRule): string {
  return rule.match.includes('\n') ? rule.match : rule.match.trim()
}

function matchOpening(rule: WafRule): string {
  return rule.match.includes('\n') ? '(\n' : '('
}

function matchExpression(rule: WafRule): string {
  const closing = rule.match.includes('\n') ? '\n)' : ')'
  return `${matchOpening(rule)}${embeddedMatch(rule)}${closing}`
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

interface RuleBlock {
  readonly rule: WafRule
  readonly ruleIndex: number
  /** Index of the block's comment line; its `if` line always follows. */
  readonly firstLine: number
  /** Index just past the block's last line. */
  readonly endLine: number
}

function ruleSpans(lines: readonly string[], blocks: readonly RuleBlock[]): WafRuleSpan[] {
  const lineStarts: number[] = []
  let offset = 0
  for (const line of lines) {
    lineStarts.push(offset)
    offset += Buffer.byteLength(line, 'utf8') + 1
  }
  return blocks.map(({ rule, ruleIndex, firstLine, endLine }) => {
    // ruleCondition always ends in matchExpression, so the `if` line ends in
    // that expression followed by the block opener.
    const ifLine = firstLine + 1
    const fromLineEnd = Buffer.byteLength(`${matchExpression(rule)}) {`, 'utf8')
    return {
      ruleIndex,
      start: lineStarts[firstLine] ?? 0,
      end: lineStarts[endLine] ?? offset,
      matchStart:
        (lineStarts[ifLine] ?? 0) +
        Buffer.byteLength(lines[ifLine] ?? '', 'utf8') -
        fromLineEnd +
        Buffer.byteLength(matchOpening(rule), 'utf8'),
    }
  })
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

  const ruleBlocks: RuleBlock[] = []
  for (const { rule, index } of orderedRules) {
    lines.push('')
    const firstLine = lines.length
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
    ruleBlocks.push({ rule, ruleIndex: index, firstLine, endLine: lines.length })
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
    ruleSpans: ruleSpans(lines, ruleBlocks),
  }
}

export interface WafScriptLocation {
  readonly ruleIndex: number
  /** 1-based position in the authored match text; absent outside of it. */
  readonly match?: { readonly line: number; readonly column: number }
}

// Maps a validator byte offset back to the rule that produced it. Parse errors
// can point just past the authored text (an unbalanced expression fails on the
// generated `{`), so offsets beyond the match clamp to its end.
export function locateScriptOffset(
  policy: WafPolicy,
  compilation: Extract<WafCompilation, { kind: 'mounted' }>,
  offset: number,
): WafScriptLocation | null {
  const span = compilation.ruleSpans.find(({ start, end }) => offset >= start && offset < end)
  const rule = span ? policy.rules[span.ruleIndex] : undefined
  if (!span || !rule) {
    return null
  }
  if (offset < span.matchStart) {
    return { ruleIndex: span.ruleIndex }
  }
  const embedded = embeddedMatch(rule)
  const consumed = Buffer.from(embedded, 'utf8')
    .subarray(0, offset - span.matchStart)
    .toString('utf8')
  const before = `${rule.match.slice(0, rule.match.indexOf(embedded))}${consumed}`.split('\n')
  return {
    ruleIndex: span.ruleIndex,
    match: { line: before.length, column: (before.at(-1)?.length ?? 0) + 1 },
  }
}
