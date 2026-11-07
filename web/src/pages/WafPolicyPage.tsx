import { lazy, Suspense, useEffect, useState, type FormEvent } from 'react'
import { Link } from 'react-router'

import { compileWafPolicyPreview } from '../api/client'
import type {
  WafCompilePreviewView,
  WafMode,
  WafRule,
  WafRuleExclusion,
  WafSeverity,
} from '../api/types'
import type { YamlCodeEditorDiagnostic } from '../components/YamlCodeEditor'
import { createUuid } from '../shared/uuid'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useWafPolicyDraft } from './useWafPolicyDraft'

const YamlCodeEditor = lazy(async () => {
  const module = await import('../components/YamlCodeEditor')
  return { default: module.YamlCodeEditor }
})

const modeLabels: Record<WafMode, string> = {
  off: '关闭',
  log: '仅记录',
  block: '拦截',
}

const severityLabels: Record<WafSeverity, string> = {
  critical: '严重',
  high: '高',
  medium: '中',
  low: '低',
}

const unmountReasonLabels: Record<string, string> = {
  'no-rules': '策略中没有规则',
  'no-mounted-projects': '没有任何 Project 挂载该函数',
}

function csvToList(value: string): string[] {
  return value
    .split(',')
    .map((item) => item.trim())
    .filter((item) => item.length > 0)
}

function newRuleId(): string {
  return `rule-${createUuid().replace(/-/g, '').slice(0, 8)}`
}

function emptyRule(): WafRule {
  return {
    id: newRuleId(),
    name: '',
    severity: 'high',
    priority: 100,
    action: 'block',
    phase: 'request_head',
    match: 'strings.contains($req.path, "/")',
  }
}

// Map compile-preview issue field paths (rules[2].match) onto editor
// diagnostics for the rule match editors.
function matchDiagnostics(
  preview: WafCompilePreviewView | null,
): Record<number, readonly YamlCodeEditorDiagnostic[]> {
  const diagnostics: Record<number, readonly YamlCodeEditorDiagnostic[]> = {}
  if (!preview) return diagnostics
  for (const issue of preview.issues) {
    const match = /^rules\[(\d+)\]\.match$/.exec(issue.field)
    if (!match?.[1]) continue
    const index = Number(match[1])
    const existing = diagnostics[index] ?? []
    diagnostics[index] = [
      ...existing,
      { line: 1, column: 1, path: issue.field, code: issue.code, message: issue.message },
    ]
  }
  return diagnostics
}

export function WafPolicyPage() {
  const { draft, policy, dirty, loading, saving, errorMessage, updatePolicy, discard, save } =
    useWafPolicyDraft()
  const [preview, setPreview] = useState<WafCompilePreviewView | null>(null)
  const [previewError, setPreviewError] = useState<string | null>(null)
  const [previewLoading, setPreviewLoading] = useState(false)

  useUnsavedChangesGuard(dirty, '当前 WAF 策略草稿尚未保存，确定离开吗？')

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

  const setRule = (index: number, next: WafRule): void =>
    updatePolicy((current) => ({
      ...current,
      rules: current.rules.map((rule, ruleIndex) => (ruleIndex === index ? next : rule)),
    }))

  const addRule = (): void =>
    updatePolicy((current) => ({ ...current, rules: [...current.rules, emptyRule()] }))

  const removeRule = (index: number): void =>
    updatePolicy((current) => ({
      ...current,
      rules: current.rules.filter((_, ruleIndex) => ruleIndex !== index),
    }))

  const diagnosticsByRule = matchDiagnostics(preview)

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
              <h3 id="waf-rules-title">规则（{policy.rules.length}/256）</h3>
              <button className="button-secondary" onClick={addRule} type="button">
                + 添加规则
              </button>
            </header>

            {policy.rules.length === 0 ? (
              <div className="route-empty-state">
                <h3>尚无规则</h3>
                <p>添加规则后，发布时会生成 WAF 边缘函数脚本。</p>
              </div>
            ) : (
              <ol className="route-list">
                {policy.rules.map((rule, index) => (
                  <RuleCard
                    diagnostics={diagnosticsByRule[index] ?? []}
                    key={rule.id}
                    rule={rule}
                    title={`规则 ${index + 1}`}
                    onRemove={() => removeRule(index)}
                    onRuleChange={(next) => setRule(index, next)}
                  />
                ))}
              </ol>
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
                    className={`status-chip status-chip-${preview.valid ? 'ready' : 'pending'}`}
                  >
                    {preview.valid ? '可编译' : `${preview.issues.length} 个问题`}
                  </span>{' '}
                  {preview.compilation.kind === 'mounted'
                    ? `挂载函数 ${preview.compilation.functionName} · 脚本 ${
                        preview.compilation.script.split('\n').length
                      } 行 · sha256 ${preview.compilation.scriptSha256.slice(0, 16)}…`
                    : `不生成脚本（${unmountReasonLabels[preview.compilation.reason] ?? preview.compilation.reason}），将发布空函数索引卸载。`}
                </p>
                {preview.compilation.kind === 'mounted' &&
                preview.compilation.directives.length > 0 ? (
                  <p>
                    CIDR 指令：
                    {preview.compilation.directives
                      .map((directive) => `${directive.name}（${directive.cidrs.length} 段）`)
                      .join('、')}
                  </p>
                ) : null}
                {preview.issues.length > 0 ? (
                  <ul className="route-issues">
                    {preview.issues.map((issue) => (
                      <li key={`${issue.field}-${issue.code}`}>
                        <span>{issue.field}</span>
                        <span>{issue.code}</span>
                        <span>{issue.message}</span>
                      </li>
                    ))}
                  </ul>
                ) : null}
                {preview.scriptErrors.length > 0 ? (
                  <ul className="route-issues">
                    {preview.scriptErrors.map((error, errorIndex) => (
                      <li key={`${error.code}-${errorIndex}`}>
                        <span>script</span>
                        <span>{error.code}</span>
                        <span>
                          {error.offset !== undefined ? `offset ${error.offset}：` : ''}
                          {error.message}
                        </span>
                      </li>
                    ))}
                  </ul>
                ) : null}
                {preview.compilation.kind === 'mounted' ? (
                  <details className="wire-preview">
                    <summary>查看生成脚本</summary>
                    <pre>{preview.compilation.script}</pre>
                  </details>
                ) : null}
              </div>
            )}
          </section>
        </>
      )}
    </section>
  )
}

interface RuleCardProps {
  rule: WafRule
  title: string
  diagnostics: readonly YamlCodeEditorDiagnostic[]
  onRuleChange(next: WafRule): void
  onRemove(): void
}

function RuleCard({ rule, title, diagnostics, onRuleChange, onRemove }: RuleCardProps) {
  const [exclusionProject, setExclusionProject] = useState('')
  const [exclusionReason, setExclusionReason] = useState('')

  const scope = rule.scope ?? {}
  const exclusions = rule.exclusions ?? []

  const addExclusion = (event: FormEvent): void => {
    event.preventDefault()
    const project = exclusionProject.trim()
    const reason = exclusionReason.trim()
    if (!project || !reason) return
    const exclusion: WafRuleExclusion = { project, reason }
    onRuleChange({
      ...rule,
      exclusions: [...exclusions.filter((existing) => existing.project !== project), exclusion],
    })
    setExclusionProject('')
    setExclusionReason('')
  }

  return (
    <li className="route-card">
      <header className="route-card-header">
        <div>
          <strong>{rule.name || '未命名规则'}</strong>
          <small>
            {rule.id} · {severityLabels[rule.severity]} · 优先级 {rule.priority} ·{' '}
            {rule.action === 'block' ? '拦截' : '记录'}
          </small>
        </div>
        <div className="route-card-actions">
          <button
            className="button-danger"
            onClick={onRemove}
            title={`移除规则 ${rule.name || rule.id}`}
            type="button"
          >
            移除规则
          </button>
        </div>
      </header>

      <div className="waf-rule-grid">
        <label>
          规则名称
          <input
            aria-label={`${title} 规则名称`}
            maxLength={255}
            value={rule.name}
            onChange={(event) => onRuleChange({ ...rule, name: event.target.value })}
          />
        </label>
        <label>
          严重级别
          <select
            aria-label={`${title} 严重级别`}
            value={rule.severity}
            onChange={(event) =>
              onRuleChange({ ...rule, severity: event.target.value as WafSeverity })
            }
          >
            {(['critical', 'high', 'medium', 'low'] as const).map((severity) => (
              <option key={severity} value={severity}>
                {severityLabels[severity]}
              </option>
            ))}
          </select>
        </label>
        <label>
          优先级
          <input
            aria-label={`${title} 优先级`}
            inputMode="numeric"
            max={1000000}
            min={0}
            type="number"
            value={rule.priority}
            onChange={(event) => onRuleChange({ ...rule, priority: Number(event.target.value) })}
          />
        </label>
        <label>
          动作
          <select
            aria-label={`${title} 动作`}
            value={rule.action}
            onChange={(event) =>
              onRuleChange({
                ...rule,
                action: event.target.value === 'log' ? 'log' : 'block',
              })
            }
          >
            <option value="block">拦截</option>
            <option value="log">记录</option>
          </select>
        </label>
      </div>

      <div className="waf-match-editor">
        <Suspense fallback={<p role="status">正在加载表达式编辑器…</p>}>
          <YamlCodeEditor
            ariaLabel={`${title} match 表达式`}
            diagnostics={diagnostics}
            language="javascript"
            onChange={(value) => onRuleChange({ ...rule, match: value })}
            onSave={() => undefined}
            value={rule.match}
          />
        </Suspense>
      </div>

      <div className="waf-rule-grid">
        <label>
          限定 Method（逗号分隔）
          <input
            aria-label={`${title} 限定 method`}
            placeholder="GET, POST"
            value={(scope.methods ?? []).join(', ')}
            onChange={(event) =>
              onRuleChange({
                ...rule,
                scope: { ...scope, methods: csvToList(event.target.value) },
              })
            }
          />
        </label>
        <label>
          限定 Path（逗号分隔）
          <input
            aria-label={`${title} 限定 path`}
            placeholder="/api/, /admin"
            value={(scope.paths ?? []).join(', ')}
            onChange={(event) =>
              onRuleChange({
                ...rule,
                scope: { ...scope, paths: csvToList(event.target.value) },
              })
            }
          />
        </label>
        <label>
          限定来源 CIDR（逗号分隔）
          <input
            aria-label={`${title} 限定来源 CIDR`}
            placeholder="10.0.0.0/8"
            value={(scope.sourceCidrs ?? []).join(', ')}
            onChange={(event) =>
              onRuleChange({
                ...rule,
                scope: { ...scope, sourceCidrs: csvToList(event.target.value) },
              })
            }
          />
        </label>
      </div>

      <div className="waf-exclusions">
        <strong>Project 豁免（按主域名，含原因可审计）</strong>
        {exclusions.length === 0 ? (
          <p className="settings-note">该规则当前对所有 Project 生效。</p>
        ) : (
          <ul>
            {exclusions.map((exclusion) => (
              <li key={exclusion.project}>
                <span>
                  <strong>{exclusion.project}</strong>
                  <small>{exclusion.reason}</small>
                </span>
                <button
                  className="button-link"
                  onClick={() =>
                    onRuleChange({
                      ...rule,
                      exclusions: exclusions.filter(
                        (existing) => existing.project !== exclusion.project,
                      ),
                    })
                  }
                  title={`移除豁免 ${exclusion.project}`}
                  type="button"
                >
                  移除豁免
                </button>
              </li>
            ))}
          </ul>
        )}
        <form className="waf-exclusion-form" onSubmit={addExclusion}>
          <input
            aria-label={`${title} 豁免项目`}
            maxLength={255}
            placeholder="Project 主域名"
            value={exclusionProject}
            onChange={(event) => setExclusionProject(event.target.value)}
          />
          <input
            aria-label={`${title} 豁免原因`}
            maxLength={255}
            placeholder="豁免原因（必填，审计可见）"
            value={exclusionReason}
            onChange={(event) => setExclusionReason(event.target.value)}
          />
          <button className="button-secondary" type="submit">
            添加豁免
          </button>
        </form>
      </div>
    </li>
  )
}
