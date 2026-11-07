import type {
  WafCompilePreviewView,
  WafGroup,
  WafMode,
  WafPolicy,
  WafRule,
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

export const modeLabels: Record<WafMode, string> = {
  off: '关闭',
  log: '仅记录',
  block: '拦截',
}

// Form state keeps what the operator typed: list and number fields are parsed
// only when building a rule, so a half-typed "GET," or an empty priority stays
// in the input instead of being normalized away on every keystroke.
export interface WafRuleForm {
  name: string
  group: string
  severity: WafSeverity
  priority: string
  action: WafRule['action']
  match: string
  methods: string
  paths: string
  sourceCidrs: string
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

export function newWafRule(group: string): WafRule {
  return {
    id: `rule-${createUuid().replace(/-/g, '').slice(0, 8)}`,
    name: '',
    group,
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
    group: rule.group,
    severity: rule.severity,
    priority: String(rule.priority),
    action: rule.action,
    match: rule.match,
    methods: (rule.scope?.methods ?? []).join(', '),
    paths: (rule.scope?.paths ?? []).join(', '),
    sourceCidrs: (rule.scope?.sourceCidrs ?? []).join(', '),
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
    group: form.group,
    severity: form.severity,
    priority: Number(form.priority),
    action: form.action,
    phase: base.phase,
    match: form.match,
    ...(Object.keys(scope).length > 0 ? { scope } : {}),
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
// other rules cannot mask it, and a log group always compiles a script even when
// the real group is off.
export function isolatedRulePolicy(rule: WafRule): WafPolicy {
  return {
    schemaVersion: 2,
    groups: [
      { id: rule.group, name: rule.group, mode: 'log', priority: 0, projects: { include: ['*'] } },
    ],
    rules: [rule],
    projectModes: {},
  }
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

// The order a group's script evaluates rules in: priority ascending, ties keep
// declaration order. Mirrors compileWafPolicy in server/src/modules/waf.
export function rulesInExecutionOrder(rules: readonly WafRule[]): readonly WafRule[] {
  return rules
    .map((rule, index) => ({ rule, index }))
    .sort((left, right) => left.rule.priority - right.rule.priority || left.index - right.index)
    .map(({ rule }) => rule)
}

// The order groups run in: mount priority ascending, ties by id.
export function groupsInExecutionOrder(groups: readonly WafGroup[]): readonly WafGroup[] {
  return [...groups].sort(
    (left, right) => left.priority - right.priority || (left.id < right.id ? -1 : 1),
  )
}

export function groupChange(group: WafGroup, saved: WafGroup | undefined): WafRuleChange {
  if (saved === undefined) return 'added'
  return JSON.stringify(group) === JSON.stringify(saved) ? null : 'modified'
}

// Mirrors star_glob in the data plane: '*' matches any run, the rest is literal.
export function projectGlobMatches(pattern: string, project: string): boolean {
  const literals = pattern.split('*').map((part) => part.replace(/[.+?^${}()|[\]\\]/gu, '\\$&'))
  return new RegExp(`^${literals.join('.*')}$`, 'su').test(project)
}

export type WafGroupMembership =
  | { state: 'active' }
  | { state: 'outside' }
  /** `exact` when this project's own entry exempts it; a glob entry may cover others too. */
  | { state: 'exempt'; project: string; reason: string; exact: boolean }

export function groupMembership(group: WafGroup, project: string): WafGroupMembership {
  if (!group.projects.include.some((glob) => projectGlobMatches(glob, project))) {
    return { state: 'outside' }
  }
  const exempt = group.projects.exempt ?? []
  const exemption =
    exempt.find((entry) => entry.project === project) ??
    exempt.find((entry) => projectGlobMatches(entry.project, project))
  return exemption
    ? { state: 'exempt', ...exemption, exact: exemption.project === project }
    : { state: 'active' }
}

export function includeSummary(group: WafGroup): string {
  const { include } = group.projects
  return include.includes('*') ? '全部 Project' : `${include.length} 条 Project 匹配`
}

// Form state keeps what the operator typed; see WafRuleForm.
export interface WafGroupForm {
  id: string
  name: string
  mode: WafMode
  priority: string
  include: string
  exempt: readonly { project: string; reason: string }[]
}

// Off until an operator turns it on: creating a group changes nothing on the wire.
export function newWafGroupForm(): WafGroupForm {
  return { id: '', name: '', mode: 'off', priority: '100', include: '*', exempt: [] }
}

export function groupToForm(group: WafGroup): WafGroupForm {
  return {
    id: group.id,
    name: group.name,
    mode: group.mode,
    priority: String(group.priority),
    include: group.projects.include.join(', '),
    exempt: group.projects.exempt ?? [],
  }
}

export function formToGroup(form: WafGroupForm): WafGroup {
  return {
    id: form.id.trim(),
    name: form.name.trim(),
    mode: form.mode,
    priority: Number(form.priority),
    projects: {
      include: parseList(form.include),
      ...(form.exempt.length > 0 ? { exempt: form.exempt } : {}),
    },
  }
}

const groupIdPattern = /^[a-z0-9](?:[a-z0-9-]{0,58}[a-z0-9])?$/u

export function groupFormProblems(
  form: WafGroupForm,
  existingIds: readonly string[],
): readonly WafRuleProblem[] {
  const problems: WafRuleProblem[] = []
  const id = form.id.trim()
  if (!groupIdPattern.test(id)) {
    problems.push({
      field: 'id',
      code: 'invalid_field',
      message: '组 ID 须为小写字母、数字或连字符，1-60 字符，且首尾为字母或数字',
    })
  } else if (existingIds.includes(id)) {
    problems.push({ field: 'id', code: 'conflict', message: `组 ID ${id} 已存在` })
  }
  if (form.name.trim().length === 0) {
    problems.push({ field: 'name', code: 'required', message: '请填写组名称' })
  }
  if (!/^\d{1,7}$/u.test(form.priority.trim()) || Number(form.priority) > 1_000_000) {
    problems.push({
      field: 'priority',
      code: 'out_of_range',
      message: '优先级须为 0–1000000 的整数',
    })
  }
  if (parseList(form.include).length === 0) {
    problems.push({ field: 'include', code: 'required', message: '请至少填写一条生效 Project' })
  }
  return problems
}
