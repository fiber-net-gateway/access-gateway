import {
  lazy,
  Suspense,
  useEffect,
  useMemo,
  useRef,
  useState,
  type FormEvent,
  type KeyboardEvent,
} from 'react'

import { compileWafPolicyPreview } from '../api/client'
import type { WafGroup, WafRule, WafSeverity } from '../api/types'
import type { YamlCodeEditorDiagnostic } from '../components/YamlCodeEditor'
import {
  actionLabels,
  formProblems,
  formToRule,
  isolatedRulePolicy,
  isolatedRuleProblems,
  ruleToForm,
  severityLabels,
  type WafRuleForm,
  type WafRuleProblem,
} from './wafRuleForm'

const YamlCodeEditor = lazy(async () => {
  const module = await import('../components/YamlCodeEditor')
  return { default: module.YamlCodeEditor }
})

const fieldLabels: Record<string, string> = {
  name: '规则名称',
  group: '规则组',
  severity: '严重级别',
  priority: '优先级',
  action: '动作',
  match: 'match',
  'scope.methods': 'Method',
  'scope.paths': 'Path',
  'scope.sourceCidrs': '来源 CIDR',
}

const focusableSelector =
  'a[href], button:not(:disabled), input:not(:disabled), select:not(:disabled), ' +
  'textarea:not(:disabled), summary, [contenteditable="true"]'

const checkDebounceMillis = 400

type RuleCheck =
  | { status: 'clean' }
  | { status: 'invalid'; problems: readonly WafRuleProblem[] }
  | { status: 'unavailable'; message: string }

async function checkRule(rule: WafRule, signal?: AbortSignal): Promise<RuleCheck> {
  try {
    const preview = await compileWafPolicyPreview(isolatedRulePolicy(rule), signal)
    const problems = isolatedRuleProblems(preview)
    return problems.length === 0 ? { status: 'clean' } : { status: 'invalid', problems }
  } catch (error) {
    return {
      status: 'unavailable',
      message: error instanceof Error ? error.message : '校验服务不可用',
    }
  }
}

export function keepFocusInside(event: KeyboardEvent<HTMLElement>): void {
  const focusable = event.currentTarget.querySelectorAll<HTMLElement>(focusableSelector)
  const first = focusable[0]
  const last = focusable[focusable.length - 1]
  if (!first || !last) return
  if (event.shiftKey && document.activeElement === first) {
    event.preventDefault()
    last.focus()
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault()
    first.focus()
  }
}

interface WafRuleDialogProps {
  mode: 'add' | 'edit'
  rule: WafRule
  groups: readonly WafGroup[]
  onApply(rule: WafRule): void
  onRemove(): void
  onClose(): void
  onDirtyChange(dirty: boolean): void
}

export function WafRuleDialog({
  mode,
  rule,
  groups,
  onApply,
  onRemove,
  onClose,
  onDirtyChange,
}: WafRuleDialogProps) {
  const [form, setForm] = useState(() => ruleToForm(rule))
  const [initialFormKey] = useState(() => JSON.stringify(ruleToForm(rule)))
  const [scopeOpen, setScopeOpen] = useState(() => rule.scope !== undefined)
  const [attempted, setAttempted] = useState(false)
  const [check, setCheck] = useState<RuleCheck | null>(null)
  const [checking, setChecking] = useState(false)
  const [applying, setApplying] = useState(false)
  const [opener] = useState(() => document.activeElement)
  const nameRef = useRef<HTMLInputElement | null>(null)

  const dirty = JSON.stringify(form) !== initialFormKey
  const localProblems = formProblems(form)
  const hasLocalProblems = localProblems.length > 0
  const candidate = formToRule(form, rule)
  const candidateKey = JSON.stringify(candidate)
  const latestCandidateKey = useRef(candidateKey)
  latestCandidateKey.current = candidateKey

  useEffect(() => {
    onDirtyChange(dirty)
  }, [dirty, onDirtyChange])

  useEffect(() => {
    nameRef.current?.focus()
    return () => {
      if (opener instanceof HTMLElement && opener.isConnected) opener.focus()
    }
  }, [opener])

  // Live check of the candidate. The previous result stays on screen until the
  // next one lands so errors don't flicker away on every keystroke.
  useEffect(() => {
    if (hasLocalProblems) {
      setCheck(null)
      setChecking(false)
      return
    }
    const controller = new AbortController()
    setChecking(true)
    const timer = window.setTimeout(() => {
      void checkRule(candidate, controller.signal).then((result) => {
        if (controller.signal.aborted) return
        setCheck(result)
        setChecking(false)
      })
    }, checkDebounceMillis)
    return () => {
      controller.abort()
      window.clearTimeout(timer)
    }
    // candidateKey is the candidate's value identity.
  }, [candidateKey, hasLocalProblems])

  const problems = useMemo<readonly WafRuleProblem[]>(
    () => [
      ...(attempted ? localProblems : []),
      ...(check?.status === 'invalid' ? check.problems : []),
    ],
    // localProblems is derived from form on every render.
    [attempted, check, form],
  )

  useEffect(() => {
    if (problems.some((problem) => problem.field.startsWith('scope.'))) setScopeOpen(true)
  }, [problems])

  const matchDiagnostics = useMemo<readonly YamlCodeEditorDiagnostic[]>(
    () =>
      problems
        .filter((problem) => problem.field === 'match')
        .map((problem) => ({
          line: problem.line ?? 1,
          column: problem.column ?? 1,
          // An empty path keeps the editor on line/column: it would otherwise
          // search the line for the word "match", as in strings.match(...).
          path: '',
          code: problem.code,
          message: problem.message,
        })),
    [problems],
  )

  const update = (patch: Partial<WafRuleForm>): void =>
    setForm((current) => ({ ...current, ...patch }))

  const invalid = (field: string): true | undefined =>
    problems.some((problem) => problem.field === field) ? true : undefined

  const requestClose = (): void => {
    // A check in flight is about to apply; closing now would still apply it.
    if (applying) return
    if (!dirty || window.confirm('放弃这条规则尚未应用的修改吗？')) onClose()
  }

  // Compiles the exact candidate again rather than trusting a debounced result
  // that may predate the last keystroke. An unreachable validator must not stop
  // authoring: publication still compiles fail-closed.
  const apply = async (): Promise<void> => {
    setAttempted(true)
    if (hasLocalProblems || applying) return
    if (mode === 'edit' && !dirty) {
      onClose()
      return
    }
    setApplying(true)
    const result = await checkRule(candidate)
    setApplying(false)
    setCheck(result)
    if (result.status !== 'invalid' && latestCandidateKey.current === candidateKey) {
      onApply(candidate)
    }
  }

  const submit = (event: FormEvent): void => {
    event.preventDefault()
    void apply()
  }

  return (
    <div className="dialog-backdrop" role="presentation">
      <form
        aria-labelledby="waf-rule-dialog-title"
        aria-modal="true"
        className="dialog-card dialog-card-wide waf-rule-dialog"
        noValidate
        onKeyDown={(event) => {
          // The editor marks Escape handled when it only closes its own popup.
          if (event.key === 'Escape' && !event.defaultPrevented) requestClose()
          if (event.key === 'Tab') keepFocusInside(event)
        }}
        onSubmit={submit}
        role="dialog"
      >
        <header className="waf-rule-dialog-header">
          <h2 id="waf-rule-dialog-title">{mode === 'add' ? '添加规则' : '编辑规则'}</h2>
          <small>
            <code>{rule.id}</code> · 应用后写入页面草稿，点击“保存策略草稿”才会保存
          </small>
        </header>

        <div className="waf-rule-dialog-body">
          <div className="waf-rule-fields">
            <label>
              规则名称
              <input
                aria-invalid={invalid('name')}
                maxLength={255}
                ref={nameRef}
                value={form.name}
                onChange={(event) => update({ name: event.target.value })}
              />
            </label>
            <label>
              规则组
              <select
                value={form.group}
                onChange={(event) => update({ group: event.target.value })}
              >
                {groups.map((group) => (
                  <option key={group.id} value={group.id}>
                    {group.name}
                  </option>
                ))}
              </select>
            </label>
            <label>
              严重级别
              <select
                value={form.severity}
                onChange={(event) => update({ severity: event.target.value as WafSeverity })}
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
                aria-invalid={invalid('priority')}
                inputMode="numeric"
                title="组内数字越小越先执行"
                value={form.priority}
                onChange={(event) => update({ priority: event.target.value })}
              />
            </label>
            <label>
              动作
              <select
                value={form.action}
                onChange={(event) =>
                  update({ action: event.target.value === 'log' ? 'log' : 'block' })
                }
              >
                {(['block', 'log'] as const).map((action) => (
                  <option key={action} value={action}>
                    {actionLabels[action]}
                  </option>
                ))}
              </select>
            </label>
          </div>

          <div className="waf-rule-match">
            <span className="waf-rule-field-label">match 表达式</span>
            <Suspense fallback={<p role="status">正在加载表达式编辑器…</p>}>
              <YamlCodeEditor
                ariaLabel="match 表达式"
                diagnostics={matchDiagnostics}
                language="javascript"
                onChange={(value) => update({ match: value })}
                onSave={() => void apply()}
                tabIndents={false}
                value={form.match}
              />
            </Suspense>
            <p className="waf-rule-check" role="status">
              {checking
                ? '正在校验…'
                : check?.status === 'clean'
                  ? '校验通过'
                  : check?.status === 'unavailable'
                    ? `暂时无法校验（${check.message}），发布时仍会完整校验。`
                    : '使用 Access 脚本语言，例如 strings.contains($req.path, "/admin")'}
            </p>
          </div>

          <details
            className="waf-rule-section"
            open={scopeOpen}
            onToggle={(event) => setScopeOpen(event.currentTarget.open)}
          >
            <summary>适用范围（可选，默认对全部请求生效）</summary>
            <div className="waf-rule-scope">
              <label>
                限定 Method（逗号分隔）
                <input
                  aria-invalid={invalid('scope.methods')}
                  placeholder="GET, POST"
                  value={form.methods}
                  onChange={(event) => update({ methods: event.target.value })}
                />
              </label>
              <label>
                限定 Path（逗号分隔）
                <input
                  aria-invalid={invalid('scope.paths')}
                  placeholder="/admin, /api/*"
                  value={form.paths}
                  onChange={(event) => update({ paths: event.target.value })}
                />
              </label>
              <label>
                限定来源 CIDR（逗号分隔）
                <input
                  aria-invalid={invalid('scope.sourceCidrs')}
                  placeholder="10.0.0.0/8"
                  value={form.sourceCidrs}
                  onChange={(event) => update({ sourceCidrs: event.target.value })}
                />
              </label>
            </div>
          </details>
        </div>

        {problems.length > 0 ? (
          <ul className="route-issues waf-rule-problems" role="alert">
            {problems.map((problem, index) => (
              <li key={`${problem.field}-${problem.code}-${index}`}>
                <span>{fieldLabels[problem.field] ?? '脚本'}</span>
                <span>{problem.code}</span>
                <span>
                  {problem.line !== undefined
                    ? `第 ${problem.line} 行第 ${problem.column ?? 1} 列：`
                    : ''}
                  {problem.message}
                </span>
              </li>
            ))}
          </ul>
        ) : null}

        <footer className="waf-rule-dialog-footer">
          {mode === 'edit' ? (
            <button className="button-danger" onClick={onRemove} type="button">
              删除规则
            </button>
          ) : (
            <span />
          )}
          <div className="dialog-actions">
            <button className="button-secondary" onClick={requestClose} type="button">
              取消
            </button>
            <button className="button-primary" disabled={applying} type="submit">
              {applying ? '校验中…' : '应用到草稿'}
            </button>
          </div>
        </footer>
      </form>
    </div>
  )
}
