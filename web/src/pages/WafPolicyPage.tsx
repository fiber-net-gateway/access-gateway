import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react'
import { Link } from 'react-router'

import { compileWafPolicyPreview, fetchWafRulePack } from '../api/client'
import type { WafCompilePreviewView, WafGroup, WafPolicy, WafRule } from '../api/types'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useWafPolicyDraft } from './useWafPolicyDraft'
import { WafGroupDialog } from './WafGroupDialog'
import { WafRuleDialog } from './WafRuleDialog'
import {
  actionLabels,
  groupChange,
  groupsInExecutionOrder,
  includeSummary,
  modeLabels,
  newWafRule,
  problemsByRuleId,
  ruleChange,
  rulesInExecutionOrder,
  scopeSummary,
  severityLabels,
  type WafRuleChange,
  type WafRuleProblem,
} from './wafRuleForm'

const MAX_RULES = 256
const MAX_GROUPS = 32

const unmountReasonLabels: Record<string, string> = {
  'no-rules': '策略中没有规则',
  'no-mounted-groups': '含规则的规则组都已关闭',
}

const skipReasonLabels: Record<string, string> = {
  off: '已关闭',
  'no-rules': '没有规则',
}

type RuleDialogTarget = { mode: 'add'; rule: WafRule } | { mode: 'edit'; ruleId: string }
type GroupDialogTarget = { mode: 'add' } | { mode: 'edit'; groupId: string }

// Issue paths are positional; groups and rules are shown by name.
function fieldLocation(
  field: string | undefined,
  policy: WafPolicy,
  functionName?: string,
): string {
  const located = /^(rules|groups)\[(\d+)\](?:\.(.+))?$/u.exec(field ?? '')
  if (located?.[2] === undefined) {
    const fallback = functionName ?? 'script'
    return field === 'payloadBase64' ? fallback : (field ?? fallback)
  }
  const target = (located[1] === 'rules' ? policy.rules : policy.groups)[Number(located[2])]
  return [target?.name || target?.id || field, located[3]].filter(Boolean).join(' · ')
}

export function WafPolicyPage() {
  const {
    draft,
    policy,
    savedPolicy,
    dirty,
    loading,
    saving,
    errorMessage,
    updatePolicy,
    discard,
    save,
  } = useWafPolicyDraft()
  const [preview, setPreview] = useState<WafCompilePreviewView | null>(null)
  const [ruleProblems, setRuleProblems] = useState<ReadonlyMap<string, readonly WafRuleProblem[]>>(
    new Map(),
  )
  const [dialogTarget, setDialogTarget] = useState<RuleDialogTarget | null>(null)
  const [groupDialogTarget, setGroupDialogTarget] = useState<GroupDialogTarget | null>(null)
  const [dialogDirty, setDialogDirty] = useState(false)
  const [previewError, setPreviewError] = useState<string | null>(null)
  const [previewLoading, setPreviewLoading] = useState(false)
  const [importingPack, setImportingPack] = useState(false)
  const [packMessage, setPackMessage] = useState<string | null>(null)

  useUnsavedChangesGuard(dirty || dialogDirty, '当前 WAF 策略草稿尚未保存，确定离开吗？')

  // Debounced live compile check: every edit revalidates the policy and
  // recompiles the script through the native validator (server side).
  useEffect(() => {
    if (policy === null) return
    const controller = new AbortController()
    const timer = window.setTimeout(() => {
      setPreviewLoading(true)
      void compileWafPolicyPreview(policy, controller.signal)
        .then((result) => {
          setPreview(result)
          setRuleProblems(problemsByRuleId(result, policy.rules))
          setPreviewError(null)
        })
        .catch((error: unknown) => {
          if (!controller.signal.aborted) {
            setPreviewError(error instanceof Error ? error.message : '编译预览失败')
          }
        })
        .finally(() => {
          if (!controller.signal.aborted) setPreviewLoading(false)
        })
    }, 600)
    return () => {
      controller.abort()
      window.clearTimeout(timer)
    }
  }, [policy])

  const closeDialog = useCallback((): void => {
    setDialogTarget(null)
    setGroupDialogTarget(null)
    setDialogDirty(false)
  }, [])

  const applyRule = (next: WafRule): void => {
    updatePolicy((current) => ({
      ...current,
      rules: current.rules.some((rule) => rule.id === next.id)
        ? current.rules.map((rule) => (rule.id === next.id ? next : rule))
        : [...current.rules, next],
    }))
    closeDialog()
  }

  const removeRule = (ruleId: string): void => {
    updatePolicy((current) => ({
      ...current,
      rules: current.rules.filter((rule) => rule.id !== ruleId),
    }))
    closeDialog()
  }

  const applyGroup = (next: WafGroup): void => {
    updatePolicy((current) => ({
      ...current,
      groups: current.groups.some((group) => group.id === next.id)
        ? current.groups.map((group) => (group.id === next.id ? next : group))
        : [...current.groups, next],
    }))
    closeDialog()
  }

  const removeGroup = (groupId: string): void => {
    updatePolicy((current) => ({
      ...current,
      groups: current.groups.filter((group) => group.id !== groupId),
      rules: current.rules.filter((rule) => rule.group !== groupId),
    }))
    closeDialog()
  }

  // Import the curated starter pack into the working copy: rules whose id
  // already exists are skipped so re-importing never duplicates or overwrites
  // operator edits, and a group is only added for rules that need it. Pack
  // groups arrive off. The result stays a dirty draft — review, then save.
  const importRulePack = (): void => {
    if (policy === null) return
    setImportingPack(true)
    void fetchWafRulePack()
      .then((pack) => {
        const existingRules = new Set(policy.rules.map((rule) => rule.id))
        const existingGroups = new Set(policy.groups.map((group) => group.id))
        const candidates = pack.rules.filter((rule) => !existingRules.has(rule.id))
        const newGroups = pack.groups
          .filter(
            (group) =>
              !existingGroups.has(group.id) && candidates.some((rule) => rule.group === group.id),
          )
          .slice(0, MAX_GROUPS - policy.groups.length)
        const usableGroups = new Set([...existingGroups, ...newGroups.map((group) => group.id)])
        const additions = candidates
          .filter((rule) => usableGroups.has(rule.group))
          .slice(0, MAX_RULES - policy.rules.length)
        const skipped = pack.rules.length - candidates.length
        const overflowed = candidates.length - additions.length
        if (additions.length > 0) {
          updatePolicy((current) => ({
            ...current,
            groups: [...current.groups, ...newGroups],
            rules: [...current.rules, ...additions],
          }))
        }
        setPackMessage(
          `已导入 ${additions.length} 条规则、${newGroups.length} 个规则组（${pack.version}），跳过已存在 ${skipped} 条` +
            (overflowed > 0 ? `，超出上限未导入 ${overflowed} 条` : '') +
            '——规则组默认关闭，导入后为未保存草稿，请检查后保存。',
        )
      })
      .catch((error: unknown) => {
        setPackMessage(
          error instanceof Error ? `规则包拉取失败：${error.message}` : '规则包拉取失败',
        )
      })
      .finally(() => setImportingPack(false))
  }

  const savedRules = useMemo(
    () => new Map(savedPolicy?.rules.map((rule) => [rule.id, rule])),
    [savedPolicy],
  )
  const savedGroups = useMemo(
    () => new Map(savedPolicy?.groups.map((group) => [group.id, group])),
    [savedPolicy],
  )
  const orderedGroups = useMemo(() => groupsInExecutionOrder(policy?.groups ?? []), [policy])
  const orderedRules = useMemo(() => rulesInExecutionOrder(policy?.rules ?? []), [policy])
  const changes = useMemo(() => {
    const rules = policy?.rules ?? []
    const currentIds = new Set(rules.map((rule) => rule.id))
    const changed = rules.map((rule) => ruleChange(rule, savedRules.get(rule.id)))
    return {
      added: changed.filter((change) => change === 'added').length,
      modified: changed.filter((change) => change === 'modified').length,
      removed: [...savedRules.keys()].filter((id) => !currentIds.has(id)).length,
    }
  }, [policy, savedRules])

  const dialogRule =
    dialogTarget?.mode === 'add'
      ? dialogTarget.rule
      : policy?.rules.find((rule) => rule.id === dialogTarget?.ruleId)
  const dialogGroup =
    groupDialogTarget?.mode === 'edit'
      ? (policy?.groups.find((group) => group.id === groupDialogTarget.groupId) ?? null)
      : null

  if (loading) {
    return <div className="route-empty-state page-loading-state">正在加载 WAF 策略…</div>
  }

  return (
    <section className="project-subpage" aria-labelledby="waf-policy-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">WAF POLICY DRAFT</p>
          <h2 id="waf-policy-title">WAF 策略</h2>
          <p>
            规则按组管理：每个规则组决定生效与豁免的 Project，并发布为一个边缘函数；match 表达式使用
            Access 脚本语言。
          </p>
        </div>
        <div className="header-actions">
          <Link className="button-secondary inline-button-link" to="/waf/releases">
            前往发布
          </Link>
        </div>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>WAF 策略操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      {policy === null ? (
        <div className="route-empty-state">WAF 策略草稿不可用。</div>
      ) : (
        <>
          <section className="project-workspace-bar" aria-label="WAF 策略工作区">
            <div>
              <strong>WAF Policy Draft</strong>
              <span>环境级单例草稿 · lockVersion {draft?.lockVersion ?? '未知'}</span>
              <span className={`status-chip status-chip-${dirty ? 'pending' : 'ready'}`}>
                {dirty ? '草稿已修改' : '草稿已同步'}
              </span>
            </div>
            <div className="header-actions">
              <button
                className="button-secondary"
                disabled={!dirty || saving}
                onClick={() => discard()}
                type="button"
              >
                放弃修改
              </button>
              <button
                className="button-primary"
                disabled={!dirty || saving}
                onClick={() => void save()}
                type="button"
              >
                {saving ? '保存中…' : '保存策略草稿'}
              </button>
            </div>
          </section>

          <section aria-labelledby="waf-groups-title">
            <header className="route-toolbar">
              <h3 id="waf-groups-title">
                规则组（{policy.groups.length}/{MAX_GROUPS}）· 规则（{policy.rules.length}/
                {MAX_RULES}）
              </h3>
              <div className="header-actions">
                <button
                  className="button-secondary"
                  disabled={importingPack}
                  onClick={importRulePack}
                  type="button"
                >
                  {importingPack ? '导入中…' : '导入起手规则包'}
                </button>
                <button
                  className="button-secondary"
                  disabled={policy.groups.length >= MAX_GROUPS}
                  onClick={() => setGroupDialogTarget({ mode: 'add' })}
                  type="button"
                >
                  + 新建规则组
                </button>
              </div>
            </header>

            {packMessage ? (
              <p className="settings-note" role="status">
                {packMessage}
              </p>
            ) : null}

            {policy.groups.length === 0 ? (
              <div className="route-empty-state">
                <h3>尚无规则组</h3>
                <p>新建规则组或导入起手规则包；规则组开启后，发布时会生成对应的边缘函数。</p>
              </div>
            ) : (
              <>
                <p className="settings-note waf-rule-list-note">
                  按执行顺序排列，优先级数字小的先执行；前面的规则组拦截后，后面的不再执行
                  {changes.added + changes.modified + changes.removed > 0
                    ? `。未保存规则：新增 ${changes.added} · 修改 ${changes.modified} · 移除 ${changes.removed}`
                    : ''}
                </p>
                <div className="waf-group-list">
                  {orderedGroups.map((group) => (
                    <GroupCard
                      addDisabled={policy.rules.length >= MAX_RULES}
                      change={groupChange(group, savedGroups.get(group.id))}
                      group={group}
                      key={group.id}
                      onAddRule={() => setDialogTarget({ mode: 'add', rule: newWafRule(group.id) })}
                      onEdit={() => setGroupDialogTarget({ mode: 'edit', groupId: group.id })}
                    >
                      {orderedRules
                        .filter((rule) => rule.group === group.id)
                        .map((rule) => (
                          <RuleRow
                            change={ruleChange(rule, savedRules.get(rule.id))}
                            key={rule.id}
                            problems={ruleProblems.get(rule.id) ?? []}
                            rule={rule}
                            onEdit={() => setDialogTarget({ mode: 'edit', ruleId: rule.id })}
                          />
                        ))}
                    </GroupCard>
                  ))}
                </div>
              </>
            )}
          </section>

          <section className="settings-card" aria-labelledby="waf-preview-title">
            <h3 id="waf-preview-title">编译预览</h3>
            <p className="settings-note">
              每次修改后自动执行完整校验与原生脚本编译（经由 server 的 Native Validator）。
            </p>
            {previewError ? (
              <div className="error-banner" role="alert">
                <strong>编译预览失败</strong>
                <span>{previewError}</span>
              </div>
            ) : previewLoading && preview === null ? (
              <p role="status">正在编译…</p>
            ) : preview === null ? null : (
              <div className="waf-compile-summary">
                <p>
                  <span
                    className={`status-chip status-chip-${
                      preview.issues.length + preview.scriptErrors.length === 0
                        ? 'ready'
                        : 'pending'
                    }`}
                  >
                    {preview.issues.length + preview.scriptErrors.length === 0
                      ? '可编译'
                      : `${preview.issues.length + preview.scriptErrors.length} 个问题`}
                  </span>{' '}
                  {preview.compilation === null
                    ? '修复下列问题后才能生成脚本。'
                    : preview.compilation.kind === 'mounted'
                      ? `将发布 ${preview.compilation.functions.length} 个边缘函数`
                      : `不生成脚本（${unmountReasonLabels[preview.compilation.reason] ?? preview.compilation.reason}），发布后不挂载 WAF 函数。`}
                </p>
                {preview.compilation !== null && preview.compilation.skipped.length > 0 ? (
                  <p>
                    不发布：
                    {preview.compilation.skipped
                      .map(
                        ({ groupId, reason }) =>
                          `${policy.groups.find((group) => group.id === groupId)?.name ?? groupId}（${skipReasonLabels[reason]}）`,
                      )
                      .join('、')}
                  </p>
                ) : null}
                {preview.issues.length > 0 ? (
                  <ul className="route-issues waf-preview-issues">
                    {preview.issues.map((issue) => (
                      <li key={`${issue.field}-${issue.code}`}>
                        <span>{fieldLocation(issue.field, policy)}</span>
                        <span>{issue.code}</span>
                        <span>{issue.message}</span>
                      </li>
                    ))}
                  </ul>
                ) : null}
                {preview.scriptErrors.length > 0 ? (
                  <ul className="route-issues waf-preview-issues">
                    {preview.scriptErrors.map((error, errorIndex) => (
                      <li key={`${error.code}-${errorIndex}`}>
                        <span>{fieldLocation(error.field, policy, error.functionName)}</span>
                        <span>{error.code}</span>
                        <span>
                          {error.line !== undefined
                            ? `第 ${error.line} 行第 ${error.column ?? 1} 列：`
                            : error.offset !== undefined
                              ? `offset ${error.offset}：`
                              : ''}
                          {error.message}
                        </span>
                      </li>
                    ))}
                  </ul>
                ) : null}
                {preview.compilation?.kind === 'mounted'
                  ? preview.compilation.functions.map((fn) => (
                      <details className="wire-preview" key={fn.functionName}>
                        <summary>
                          {fn.functionName} · {fn.ruleOrder.length} 条规则 · 脚本{' '}
                          {fn.script.split('\n').length} 行
                          {fn.directives.length > 0 ? ` · CIDR 指令 ${fn.directives.length}` : ''} ·
                          sha256 {fn.scriptSha256.slice(0, 16)}…
                        </summary>
                        <pre>{fn.script}</pre>
                      </details>
                    ))
                  : null}
              </div>
            )}
          </section>

          {dialogTarget !== null && dialogRule !== undefined ? (
            <WafRuleDialog
              key={dialogRule.id}
              mode={dialogTarget.mode}
              rule={dialogRule}
              groups={orderedGroups}
              onApply={applyRule}
              onClose={closeDialog}
              onDirtyChange={setDialogDirty}
              onRemove={() => removeRule(dialogRule.id)}
            />
          ) : null}

          {groupDialogTarget !== null ? (
            <WafGroupDialog
              key={dialogGroup?.id ?? 'new'}
              existingIds={policy.groups.map((group) => group.id)}
              group={dialogGroup}
              ruleCount={policy.rules.filter((rule) => rule.group === dialogGroup?.id).length}
              onApply={applyGroup}
              onClose={closeDialog}
              onDirtyChange={setDialogDirty}
              onRemove={() => dialogGroup && removeGroup(dialogGroup.id)}
            />
          ) : null}
        </>
      )}
    </section>
  )
}

const changeChips: Record<NonNullable<WafRuleChange>, { label: string; tone: string }> = {
  added: { label: '新增', tone: 'ready' },
  modified: { label: '已修改', tone: 'unknown' },
}

const groupModeChips: Record<WafGroup['mode'], string> = {
  off: 'unknown',
  log: 'pending',
  block: 'ready',
}

interface GroupCardProps {
  group: WafGroup
  change: WafRuleChange
  addDisabled: boolean
  onEdit(): void
  onAddRule(): void
  children: ReactNode[]
}

function GroupCard({ group, change, addDisabled, onEdit, onAddRule, children }: GroupCardProps) {
  const exemptCount = group.projects.exempt?.length ?? 0
  return (
    <section aria-label={`规则组 ${group.name}`} className="waf-group">
      <header className="waf-group-header">
        <div className="waf-rule-row-main">
          <div className="waf-rule-row-title">
            <strong title={group.name}>{group.name}</strong>
            <span className={`status-chip status-chip-${groupModeChips[group.mode]}`}>
              {modeLabels[group.mode]}
            </span>
            {change !== null ? (
              <span className={`status-chip status-chip-${changeChips[change].tone}`}>
                {changeChips[change].label}
              </span>
            ) : null}
          </div>
          <code className="waf-rule-row-detail">
            waf-{group.id} · 优先级 {group.priority} · {includeSummary(group)} · 豁免 {exemptCount}
          </code>
        </div>
        <div className="header-actions">
          <button
            aria-label={`编辑规则组 ${group.name}`}
            className="button-secondary"
            onClick={onEdit}
            type="button"
          >
            编辑组
          </button>
          <button
            aria-label={`向 ${group.name} 添加规则`}
            className="button-secondary"
            disabled={addDisabled}
            onClick={onAddRule}
            type="button"
          >
            + 添加规则
          </button>
        </div>
      </header>
      {children.length > 0 ? (
        <ol className="waf-rule-list">{children}</ol>
      ) : (
        <p className="settings-note waf-group-empty">本组还没有规则，发布时不会生成函数。</p>
      )}
    </section>
  )
}

interface RuleRowProps {
  rule: WafRule
  change: WafRuleChange
  problems: readonly WafRuleProblem[]
  onEdit(): void
}

function RuleRow({ rule, change, problems, onEdit }: RuleRowProps) {
  return (
    <li className="waf-rule-row">
      <span className={`status-chip waf-severity-${rule.severity}`}>
        {severityLabels[rule.severity]}
      </span>
      <div className="waf-rule-row-main">
        <div className="waf-rule-row-title">
          <strong title={rule.name}>{rule.name}</strong>
          {change !== null ? (
            <span className={`status-chip status-chip-${changeChips[change].tone}`}>
              {changeChips[change].label}
            </span>
          ) : null}
          {problems.length > 0 ? (
            <span
              className="status-chip status-chip-degraded"
              title={problems.map((problem) => problem.message).join('\n')}
            >
              {problems.length} 个问题
            </span>
          ) : null}
        </div>
        <code className="waf-rule-row-detail" title={rule.match}>
          {rule.id} · {rule.match}
        </code>
      </div>
      <div className="waf-rule-row-facts">
        <span>{actionLabels[rule.action]}</span>
        <span>优先级 {rule.priority}</span>
        <span title={scopeSummary(rule)}>{scopeSummary(rule)}</span>
      </div>
      <button
        aria-label={`编辑规则 ${rule.name}`}
        className="button-secondary"
        onClick={onEdit}
        type="button"
      >
        编辑
      </button>
    </li>
  )
}
