// WAF policy → edge-function compilation (docs/edge-functions-and-waf-design.md
// §4.2). Pure and deterministic: the same policy always compiles to the same
// script bytes, so a script sha256 (the wire identity) changes only when its
// group's semantics change.
//
// Each group with rules and a mode other than off compiles to one edge function
// `waf-<group id>`: the group's project selector becomes the wire mount, and
// its rules become the script. Shape of a generated script:
//   - `cidr` directive declarations at the top (shared, deduplicated across
//     the group's rules by identical CIDR set, named net1..netN in canonical
//     order);
//   - one `if` per rule in priority order (ties keep declaration order),
//     condition parts ordered cheapest-first: methods, path prefix/equality
//     chain, CIDR membership, then the authored match expression (always
//     parenthesized);
//   - `action: block` rules gate on the project's effective mode: block mode
//     reports `waf.block`, answers 403 JSON and returns, so one request is
//     blocked once; log mode reports `waf.log` only (no response is sent);
//   - `action: log` rules always report `waf.log`.
//
// Preconditions: the policy passed validateWafPolicy (charset/enum/shape
// guarantees this module relies on, e.g. comment-safe display names and
// round-trippable string escaping).

import { createHash } from 'node:crypto'

import {
  groupModeSets,
  groupMountSelector,
  type WafModeSets,
  type WafMountSelector,
} from './group-scope.js'
import type { WafGroup, WafPolicy, WafRule } from './model.js'
import { wafFunctionName } from './model.js'
import { cidrDirectiveLiteral, dialectStringLiteral } from './escaper.js'

export interface WafDirective {
  readonly name: string
  readonly cidrs: readonly string[]
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

export interface WafFunctionCompilation {
  readonly groupId: string
  readonly functionName: string
  readonly script: string
  readonly scriptSha256: string
  readonly mountPriority: number
  readonly mounts: readonly WafMountSelector[]
  /** Rule ids in generated execution order (priority asc, declaration ties). */
  readonly ruleOrder: readonly string[]
  readonly directives: readonly WafDirective[]
  readonly ruleSpans: readonly WafRuleSpan[]
}

/** A group that publishes nothing, and why. */
export interface WafSkippedGroup {
  readonly groupId: string
  readonly reason: 'off' | 'no-rules'
}

export type WafCompilation =
  | {
      readonly kind: 'unmount'
      readonly reason: 'no-rules' | 'no-mounted-groups'
      readonly skipped: readonly WafSkippedGroup[]
    }
  | {
      readonly kind: 'mounted'
      /** Ascending mount priority, ties by group id: the wire declaration order. */
      readonly functions: readonly WafFunctionCompilation[]
      readonly skipped: readonly WafSkippedGroup[]
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
    `${indent}return true;`,
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

function compileGroup(
  group: WafGroup,
  sets: WafModeSets,
  rules: readonly { rule: WafRule; index: number }[],
): WafFunctionCompilation {
  const functionName = wafFunctionName(group.id)

  // Directives: deduplicate by canonical (sorted) CIDR set, then name them in
  // canonical order so author reorderings keep the script hash stable.
  const cidrSets = new Map<string, string[]>()
  for (const { rule } of rules) {
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
  // execute: they are excluded from the mount). Include globs leave the full
  // project list unknown here, so a non-default mode is "possible" — any
  // project without an override carries the group mode. The runtime gate
  // expression below decides per request; these flags only choose which
  // branches to emit.
  const blockPossible = group.mode === 'block' || sets.block.length > 0
  const logPossible = group.mode === 'log' || sets.log.length > 0

  const lines: string[] = []
  lines.push(
    `// ${functionName}: generated by the access-gateway console from WAF group ${group.id}; do not edit.`,
  )
  lines.push(
    `// rules=${String(rules.length)} group-mode=${group.mode} explicit-modes block=${String(sets.block.length)}/log=${String(sets.log.length)}/off=${String(sets.off.length)}`,
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
      group.mode === 'block'
        ? projectNotInEnumeration(sets.log)!
        : projectInEnumeration(sets.block)!
  }

  const orderedRules = [...rules].sort((left, right) =>
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
    groupId: group.id,
    functionName,
    script,
    scriptSha256,
    mountPriority: group.priority,
    mounts: [groupMountSelector(group, sets)],
    ruleOrder: orderedRules.map(({ rule }) => rule.id),
    directives,
    ruleSpans: ruleSpans(lines, ruleBlocks),
  }
}

export function compileWafPolicy(policy: WafPolicy): WafCompilation {
  const functions: WafFunctionCompilation[] = []
  const skipped: WafSkippedGroup[] = []
  const indexedRules = policy.rules.map((rule, index) => ({ rule, index }))
  const groups = [...policy.groups].sort(
    (left, right) => left.priority - right.priority || (left.id < right.id ? -1 : 1),
  )
  for (const group of groups) {
    const rules = indexedRules.filter(({ rule }) => rule.group === group.id)
    if (group.mode === 'off') {
      skipped.push({ groupId: group.id, reason: 'off' })
    } else if (rules.length === 0) {
      skipped.push({ groupId: group.id, reason: 'no-rules' })
    } else {
      functions.push(compileGroup(group, groupModeSets(policy, group), rules))
    }
  }
  if (functions.length > 0) {
    return { kind: 'mounted', functions, skipped }
  }
  return {
    kind: 'unmount',
    reason: policy.rules.length === 0 ? 'no-rules' : 'no-mounted-groups',
    skipped,
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
  compilation: WafFunctionCompilation,
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
