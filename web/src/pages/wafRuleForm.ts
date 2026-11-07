import type {
  WafCompilePreviewView,
  WafPolicy,
  WafRule,
  WafRuleExclusion,
  WafSeverity,
} from '../api/types'
import { createUuid } from '../shared/uuid'

export const severityLabels: Record<WafSeverity, string> = {
  critical: '严重',
  high: '高',
  medium: '中',
  low: '低',
}

export const actionLabels: Record<WafRule['action'], string> = {
  block: '拦截',
  log: '记录',
}

// Form state keeps what the operator typed: list and number fields are parsed
// only when building a rule, so a half-typed "GET," or an empty priority stays
// in the input instead of being normalized away on every keystroke.
export interface WafRuleForm {
  name: string
  severity: WafSeverity
  priority: string
  action: WafRule['action']
  match: string
  methods: string
  paths: string
  sourceCidrs: string
  exclusions: readonly WafRuleExclusion[]
}

// One problem on one rule; `field` is relative to the rule ("scope.methods"),
// and empty when the error belongs to the rule as a whole.
export interface WafRuleProblem {
  field: string
  code: string
  message: string
  line?: number
  column?: number
}

export function newWafRule(): WafRule {
  return {
    id: `rule-${createUuid().replace(/-/g, '').slice(0, 8)}`,
    name: '',
    severity: 'high',
    priority: 100,
    action: 'block',
    phase: 'request_head',
    match: '',
  }
}

export function ruleToForm(rule: WafRule): WafRuleForm {
  return {
    name: rule.name,
    severity: rule.severity,
    priority: String(rule.priority),
    action: rule.action,
    match: rule.match,
    methods: (rule.scope?.methods ?? []).join(', '),
    paths: (rule.scope?.paths ?? []).join(', '),
    sourceCidrs: (rule.scope?.sourceCidrs ?? []).join(', '),
    exclusions: rule.exclusions ?? [],
  }
}

function parseList(value: string): string[] {
  return value.split(/[,，\s]+/u).filter((item) => item.length > 0)
}

export function formToRule(form: WafRuleForm, base: WafRule): WafRule {
  const methods = parseList(form.methods).map((method) => method.toUpperCase())
  const paths = parseList(form.paths)
  const sourceCidrs = parseList(form.sourceCidrs)
  const scope = {
    ...(methods.length > 0 ? { methods } : {}),
    ...(paths.length > 0 ? { paths } : {}),
    ...(sourceCidrs.length > 0 ? { sourceCidrs } : {}),
  }
  return {
    id: base.id,
    name: form.name.trim(),
    severity: form.severity,
    priority: Number(form.priority),
    action: form.action,
    phase: base.phase,
    match: form.match,
    ...(Object.keys(scope).length > 0 ? { scope } : {}),
    ...(form.exclusions.length > 0 ? { exclusions: form.exclusions } : {}),
  }
}

// What can be answered without a round trip. Everything else is the server
// validation's call, so its rules are not mirrored here.
export function formProblems(form: WafRuleForm): readonly WafRuleProblem[] {
  const problems: WafRuleProblem[] = []
  if (form.name.trim().length === 0) {
    problems.push({ field: 'name', code: 'required', message: '请填写规则名称' })
  }
  if (!/^\d{1,7}$/u.test(form.priority.trim()) || Number(form.priority) > 1_000_000) {
    problems.push({
      field: 'priority',
      code: 'out_of_range',
      message: '优先级须为 0–1000000 的整数',
    })
  }
  if (form.match.trim().length === 0) {
    problems.push({ field: 'match', code: 'required', message: '请填写 match 表达式' })
  }
  return problems
}

// A policy holding only this rule: every issue it yields belongs to the rule,
// other rules cannot mask it, and log mode always compiles a script even when
// the real default mode is off.
export function isolatedRulePolicy(rule: WafRule): WafPolicy {
  return { schemaVersion: 1, defaultMode: 'log', rules: [rule], projectModes: {} }
}

const ruleFieldPattern = /^rules\[(\d+)\](?:\.(.+))?$/u

function ruleProblem(
  field: string | undefined,
  rest: Omit<WafRuleProblem, 'field'>,
): { ruleIndex: number | null; problem: WafRuleProblem } {
  const located = ruleFieldPattern.exec(field ?? '')
  return {
    ruleIndex: located?.[1] === undefined ? null : Number(located[1]),
    problem: { ...rest, field: located?.[2] ?? '' },
  }
}

function locatedProblems(preview: WafCompilePreviewView) {
  return [
    ...preview.issues.map(({ field, code, message }) => ruleProblem(field, { code, message })),
    ...preview.scriptErrors.map(({ field, code, message, line, column }) =>
      ruleProblem(field, {
        code,
        message,
        ...(line !== undefined ? { line } : {}),
        ...(column !== undefined ? { column } : {}),
      }),
    ),
  ]
}

// For a compile of isolatedRulePolicy: nothing else can have caused a problem.
export function isolatedRuleProblems(preview: WafCompilePreviewView): readonly WafRuleProblem[] {
  return locatedProblems(preview).map(({ problem }) => problem)
}

// For a compile of the whole policy. Keyed by rule id so a removal or reorder
// can't shift problems onto a neighbour while the next preview is in flight.
export function problemsByRuleId(
  preview: WafCompilePreviewView,
  rules: readonly WafRule[],
): ReadonlyMap<string, readonly WafRuleProblem[]> {
  const byRule = new Map<string, WafRuleProblem[]>()
  for (const { ruleIndex, problem } of locatedProblems(preview)) {
    const id = ruleIndex === null ? undefined : rules[ruleIndex]?.id
    if (id !== undefined) byRule.set(id, [...(byRule.get(id) ?? []), problem])
  }
  return byRule
}

export function scopeSummary(rule: WafRule): string {
  const parts = [
    rule.scope?.methods?.length ? rule.scope.methods.join('/') : null,
    rule.scope?.paths?.length ? `${rule.scope.paths.length} 条路径` : null,
    rule.scope?.sourceCidrs?.length ? `${rule.scope.sourceCidrs.length} 段 CIDR` : null,
  ].filter((part) => part !== null)
  return parts.length > 0 ? parts.join(' · ') : '全部请求'
}

export type WafRuleChange = 'added' | 'modified' | null

export function ruleChange(rule: WafRule, saved: WafRule | undefined): WafRuleChange {
  if (saved === undefined) return 'added'
  return JSON.stringify(rule) === JSON.stringify(saved) ? null : 'modified'
}

// The order the generated script evaluates rules in: priority ascending, ties
// keep declaration order. Mirrors compileWafPolicy in server/src/modules/waf.
export function rulesInExecutionOrder(rules: readonly WafRule[]): readonly WafRule[] {
  return rules
    .map((rule, index) => ({ rule, index }))
    .sort((left, right) => left.rule.priority - right.rule.priority || left.index - right.index)
    .map(({ rule }) => rule)
}
