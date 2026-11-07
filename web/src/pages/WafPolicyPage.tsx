import { useCallback, useEffect, useMemo, useState } from 'react'
import { Link } from 'react-router'

import { compileWafPolicyPreview, fetchWafRulePack } from '../api/client'
import type { WafCompilePreviewView, WafMode, WafRule } from '../api/types'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useWafPolicyDraft } from './useWafPolicyDraft'
import { WafRuleDialog } from './WafRuleDialog'
import {
  actionLabels,
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

const modeLabels: Record<WafMode, string> = {
  off: '关闭',
  log: '仅记录',
  block: '拦截',
}

const unmountReasonLabels: Record<string, string> = {
  'no-rules': '策略中没有规则',
  'no-mounted-projects': '没有任何 Project 挂载该函数',
}

type RuleDialogTarget = { mode: 'add'; rule: WafRule } | { mode: 'edit'; ruleId: string }

// Issue paths are positional; rules are shown by name and in execution order.
function fieldLocation(field: string | undefined, rules: readonly WafRule[]): string {
  const located = /^rules\[(\d+)\](?:\.(.+))?$/u.exec(field ?? '')
  if (located?.[1] === undefined) return field === 'payloadBase64' ? 'script' : (field ?? 'script')
  const rule = rules[Number(located[1])]
  return [rule?.name || rule?.id || field, located[2]].filter(Boolean).join(' · ')
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

  // Import the curated starter pack into the working copy: rules whose id
  // already exists are skipped so re-importing never duplicates or overwrites
  // operator edits. The result stays a dirty draft — review, then save.
  const importRulePack = (): void => {
    if (policy === null) return
    setImportingPack(true)
    void fetchWafRulePack()
      .then((pack) => {
        const existing = new Set(policy.rules.map((rule) => rule.id))
        const room = MAX_RULES - policy.rules.length
        const additions: WafRule[] = []
        let skipped = 0
        let overflowed = 0
        for (const rule of pack.rules) {
          if (existing.has(rule.id)) {
            skipped += 1
          } else if (additions.length < room) {
            additions.push(rule)
          } else {
            overflowed += 1
          }
        }
        if (additions.length > 0) {
          updatePolicy((current) => ({ ...current, rules: [...current.rules, ...additions] }))
        }
        setPackMessage(
          `已导入 ${additions.length} 条（${pack.version}），跳过已存在 ${skipped} 条` +
            (overflowed > 0 ? `，超出 256 条上限未导入 ${overflowed} 条` : '') +
            '——导入后为未保存草稿，请检查后保存。',
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

  if (loading) {
    return <div className="route-empty-state page-loading-state">正在加载 WAF 策略…</div>
  }

  return (
    <section className="project-subpage" aria-labelledby="waf-policy-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">WAF POLICY DRAFT</p>
          <h2 id="waf-policy-title">WAF 策略</h2>
          <p>规则保存为草稿后，由发布页编译成边缘函数脚本；match 表达式使用 Access 脚本语言。</p>
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

          <section className="settings-card" aria-labelledby="waf-default-mode-title">
            <h3 id="waf-default-mode-title">默认模式</h3>
            <p className="settings-note">
              未单独设置模式的 Project 使用默认模式；每个 Project 可在自己的 WAF
              策略页覆盖或申请豁免。
            </p>
            <div className="policy-option" role="radiogroup" aria-label="默认模式">
              {(['off', 'log', 'block'] as const).map((mode) => (
                <label key={mode}>
                  <input
                    checked={policy.defaultMode === mode}
                    name="waf-default-mode"
                    onChange={() => updatePolicy((current) => ({ ...current, defaultMode: mode }))}
                    type="radio"
                    value={mode}
                  />
                  {modeLabels[mode]}
                </label>
              ))}
            </div>
          </section>

          <section aria-labelledby="waf-rules-title">
            <header className="route-toolbar">
              <h3 id="waf-rules-title">
                规则（{policy.rules.length}/{MAX_RULES}）
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
                  disabled={policy.rules.length >= MAX_RULES}
                  onClick={() => setDialogTarget({ mode: 'add', rule: newWafRule() })}
                  type="button"
                >
                  + 添加规则
                </button>
              </div>
            </header>

            {packMessage ? (
              <p className="settings-note" role="status">
                {packMessage}
              </p>
            ) : null}

            {policy.rules.length === 0 ? (
              <div className="route-empty-state">
                <h3>尚无规则</h3>
                <p>添加规则后，发布时会生成 WAF 边缘函数脚本。</p>
              </div>
            ) : (
              <>
                <p className="settings-note waf-rule-list-note">
                  按执行顺序排列，优先级数字小的先执行
                  {changes.added + changes.modified + changes.removed > 0
                    ? `。未保存：新增 ${changes.added} · 修改 ${changes.modified} · 移除 ${changes.removed}`
                    : ''}
                </p>
                <ol className="waf-rule-list">
                  {orderedRules.map((rule) => (
                    <RuleRow
                      change={ruleChange(rule, savedRules.get(rule.id))}
                      key={rule.id}
                      problems={ruleProblems.get(rule.id) ?? []}
                      rule={rule}
                      onEdit={() => setDialogTarget({ mode: 'edit', ruleId: rule.id })}
                    />
                  ))}
                </ol>
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
                      ? `挂载函数 ${preview.compilation.functionName} · 脚本 ${
                          preview.compilation.script.split('\n').length
                        } 行 · sha256 ${preview.compilation.scriptSha256.slice(0, 16)}…`
                      : `不生成脚本（${unmountReasonLabels[preview.compilation.reason] ?? preview.compilation.reason}），将发布空函数索引卸载。`}
                </p>
                {preview.compilation?.kind === 'mounted' &&
                preview.compilation.directives.length > 0 ? (
                  <p>
                    CIDR 指令：
                    {preview.compilation.directives
                      .map((directive) => `${directive.name}（${directive.cidrs.length} 段）`)
                      .join('、')}
                  </p>
                ) : null}
                {preview.issues.length > 0 ? (
                  <ul className="route-issues waf-preview-issues">
                    {preview.issues.map((issue) => (
                      <li key={`${issue.field}-${issue.code}`}>
                        <span>{fieldLocation(issue.field, policy.rules)}</span>
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
                        <span>{fieldLocation(error.field, policy.rules)}</span>
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
                {preview.compilation?.kind === 'mounted' ? (
                  <details className="wire-preview">
                    <summary>查看生成脚本</summary>
                    <pre>{preview.compilation.script}</pre>
                  </details>
                ) : null}
              </div>
            )}
          </section>

          {dialogTarget !== null && dialogRule !== undefined ? (
            <WafRuleDialog
              key={dialogRule.id}
              mode={dialogTarget.mode}
              rule={dialogRule}
              onApply={applyRule}
              onClose={closeDialog}
              onDirtyChange={setDialogDirty}
              onRemove={() => removeRule(dialogRule.id)}
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

interface RuleRowProps {
  rule: WafRule
  change: WafRuleChange
  problems: readonly WafRuleProblem[]
  onEdit(): void
}

function RuleRow({ rule, change, problems, onEdit }: RuleRowProps) {
  const exclusionCount = rule.exclusions?.length ?? 0
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
        <span>豁免 {exclusionCount}</span>
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
