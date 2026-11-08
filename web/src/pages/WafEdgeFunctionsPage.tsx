import { lazy, Suspense, useCallback, useEffect, useState } from 'react'

import { compileWafEdgeFunctionsPreview, fetchWafEdgeFunctions } from '../api/client'
import type {
  WafEdgeFunctionCompilePreviewView,
  WafEdgeFunctionDraftFunction,
  WafEdgeFunctionsView,
} from '../api/types'
import type { YamlCodeEditorDiagnostic } from '../components/YamlCodeEditor'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useEdgeFunctionsDraft } from './useEdgeFunctionsDraft'

const YamlCodeEditor = lazy(async () => {
  const module = await import('../components/YamlCodeEditor')
  return { default: module.YamlCodeEditor }
})

const maxFunctions = 64
const fullRatio = 10000
const functionNamePattern = /^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/u

function csvToList(value: string): string[] {
  return value
    .split(',')
    .map((item) => item.trim())
    .filter((item) => item.length > 0)
}

function newFunction(name: string): WafEdgeFunctionDraftFunction {
  return {
    name,
    enabled: true,
    script: 'return true;',
    projects: { include: ['*'] },
  }
}

// `auth` → `auth-v2`, `auth-v2` → `auth-v3`, skipping names already taken. A
// version slug is the only way to run two scripts side by side: the name is
// the function's identity and derives its script Data ID.
function nextVersionName(name: string, taken: ReadonlySet<string>): string {
  const match = /^(.+)-v(\d+)$/u.exec(name)
  const base = match?.[1] ?? name
  for (let version = match ? Number(match[2]) + 1 : 2; ; version += 1) {
    const suffix = `-v${version}`
    const candidate = `${base.slice(0, 64 - suffix.length).replace(/-+$/u, '')}${suffix}`
    if (!taken.has(candidate)) return candidate
  }
}

function selectorKey(projects: WafEdgeFunctionDraftFunction['projects']): string {
  const sorted = (list: readonly string[]): string => [...list].sort().join(',')
  return `${sorted(projects.include)}|${sorted(projects.exclude ?? [])}`
}

// Promotion deletes the replaced function, so it must be a pure cleanup: at
// full ratio over the same projects the target already runs nowhere, and
// removing it changes no request. Anything else would silently unmount the
// target from projects the canary does not cover.
function promotionBlocker(
  canary: WafEdgeFunctionDraftFunction,
  target: WafEdgeFunctionDraftFunction | undefined,
): string | null {
  if (!target) return `草稿中没有函数 ${canary.replaces ?? ''}`
  if (!canary.enabled) return '请先启用本函数'
  if ((canary.ratio ?? fullRatio) !== fullRatio) return '请先把采样比例升到 10000'
  if (selectorKey(canary.projects) !== selectorKey(target.projects)) {
    return `挂载范围需与 ${target.name} 一致`
  }
  return null
}

function rolloutSummary(fn: WafEdgeFunctionDraftFunction): string {
  const ratio = fn.ratio ?? fullRatio
  if (fn.replaces) return `灰度 ${ratio}/10000 取代 ${fn.replaces}`
  return ratio === fullRatio ? '全量' : `采样 ${ratio}/10000`
}

// Byte offset → 1-based line/column for editor diagnostics.
function offsetToPosition(
  script: string,
  offset: number | undefined,
): { line: number; column: number } {
  if (offset === undefined || offset < 0 || offset > script.length) return { line: 1, column: 1 }
  const lines = script.slice(0, offset).split('\n')
  return { line: lines.length, column: (lines[lines.length - 1]?.length ?? 0) + 1 }
}

// Script-editor diagnostics for the selected function: draft-level issues
// (field paths functions[i].script) plus the native validator's per-function
// compile errors with byte offsets mapped onto line/column.
function scriptDiagnostics(
  preview: WafEdgeFunctionCompilePreviewView | null,
  index: number,
  fn: WafEdgeFunctionDraftFunction,
): readonly YamlCodeEditorDiagnostic[] {
  if (!preview) return []
  const draftIssues = preview.issues
    .filter((issue) => issue.field.startsWith(`functions[${index}].script`))
    .map((issue) => ({
      line: 1,
      column: 1,
      path: issue.field,
      code: issue.code,
      message: issue.message,
    }))
  const compileErrors = (preview.functions.find((entry) => entry.name === fn.name)?.errors ?? [])
    .filter((error) => error.offset !== undefined || error.field === 'script' || !error.field)
    .map((error) => {
      const position = offsetToPosition(fn.script, error.offset)
      return {
        line: position.line,
        column: position.column,
        path: `functions[${index}].script`,
        code: error.code,
        message: error.message,
      }
    })
  return [...draftIssues, ...compileErrors]
}

export function WafEdgeFunctionsPage() {
  const { draft, functions, dirty, loading, saving, errorMessage, updateFunctions, discard, save } =
    useEdgeFunctionsDraft()
  const [selectedName, setSelectedName] = useState<string | null>(null)
  const [newName, setNewName] = useState('')
  const [nameError, setNameError] = useState<string | null>(null)
  const [preview, setPreview] = useState<WafEdgeFunctionCompilePreviewView | null>(null)
  const [previewError, setPreviewError] = useState<string | null>(null)
  const [previewLoading, setPreviewLoading] = useState(false)

  useUnsavedChangesGuard(dirty, '当前边缘函数草稿尚未保存，确定离开吗？')

  const selectedIndex = functions.findIndex((fn) => fn.name === selectedName)
  const selected = selectedIndex >= 0 ? (functions[selectedIndex] ?? null) : (functions[0] ?? null)

  const updateSelected = (next: WafEdgeFunctionDraftFunction): void =>
    updateFunctions((current) => current.map((fn) => (fn.name === selected?.name ? next : fn)))

  // Debounced compile preview over the whole working draft — the same
  // per-script native validation a release would run, minus the WAF function.
  useEffect(() => {
    const controller = new AbortController()
    const timer = window.setTimeout(() => {
      setPreviewLoading(true)
      void compileWafEdgeFunctionsPreview({ schemaVersion: 1, functions }, controller.signal)
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
  }, [functions])

  const addFunction = (): void => {
    const name = newName.trim()
    if (!functionNamePattern.test(name)) {
      setNameError('函数名需为小写字母/数字/连字符，1-64 字符，且首尾为字母或数字')
      return
    }
    if (functions.some((fn) => fn.name === name) || name === 'waf' || name.startsWith('waf-')) {
      setNameError('函数名已存在（waf 与 waf-* 为生成函数保留）')
      return
    }
    updateFunctions((current) => [...current, newFunction(name)])
    setSelectedName(name)
    setNewName('')
    setNameError(null)
  }

  const removeFunction = (name: string): void => {
    updateFunctions((current) => current.filter((fn) => fn.name !== name))
    if (selected?.name === name) setSelectedName(null)
  }

  // A new version starts as a 1% canary of the source, same script and scope.
  const createCanary = (source: WafEdgeFunctionDraftFunction): void => {
    const name = nextVersionName(source.name, new Set(functions.map((fn) => fn.name)))
    updateFunctions((current) => [
      ...current,
      { ...source, name, ratio: 100, replaces: source.name },
    ])
    setSelectedName(name)
  }

  const promote = (canary: WafEdgeFunctionDraftFunction): void => {
    updateFunctions((current) =>
      current
        .filter((fn) => fn.name !== canary.replaces)
        .map((fn) =>
          fn.name === canary.name ? { ...fn, ratio: undefined, replaces: undefined } : fn,
        ),
    )
  }

  if (loading) {
    return <div className="route-empty-state page-loading-state">正在加载边缘函数…</div>
  }

  return (
    <section className="project-subpage" aria-labelledby="waf-edge-functions-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">EDGE FUNCTIONS · DRAFT</p>
          <h2 id="waf-edge-functions-title">边缘函数</h2>
          <p>
            手写边缘函数与 WAF 生成函数在发布时合并为同一个函数索引；脚本使用 Access
            脚本语言，每次修改自动经原生编译器逐函数校验。
          </p>
        </div>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>边缘函数操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      <section className="project-workspace-bar" aria-label="边缘函数工作区">
        <div>
          <strong>Edge Functions Draft</strong>
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
            {saving ? '保存中…' : '保存函数草稿'}
          </button>
        </div>
      </section>

      <section aria-labelledby="waf-edge-function-list-title">
        <header className="route-toolbar">
          <h3 id="waf-edge-function-list-title">
            函数（{functions.length}/{maxFunctions}）
          </h3>
          <div className="header-actions">
            <input
              aria-label="新函数名"
              maxLength={64}
              placeholder="例如 bot-trap"
              value={newName}
              onChange={(event) => {
                setNewName(event.target.value)
                setNameError(null)
              }}
            />
            <button
              className="button-secondary"
              disabled={functions.length >= maxFunctions}
              onClick={addFunction}
              type="button"
            >
              + 新增函数
            </button>
          </div>
        </header>
        {nameError ? (
          <p className="settings-note" role="alert">
            {nameError}
          </p>
        ) : null}

        {functions.length === 0 ? (
          <div className="route-empty-state">
            <h3>尚无手写函数</h3>
            <p>新增函数后保存草稿；发布时会与 WAF 生成函数一起写入函数索引。</p>
          </div>
        ) : (
          <div className="release-resources">
            {functions.map((fn) => (
              <button
                aria-pressed={selected?.name === fn.name}
                className="button-link"
                key={fn.name}
                onClick={() => setSelectedName(fn.name)}
                type="button"
              >
                <span>
                  <strong>{fn.name}</strong>
                  <small>
                    {fn.enabled ? '已启用' : '已停用'} ·{' '}
                    {fn.phase === 'request_body' ? '请求体' : '请求头'} · {rolloutSummary(fn)} ·{' '}
                    {fn.script.split('\n').length} 行脚本
                  </small>
                </span>
              </button>
            ))}
          </div>
        )}
      </section>

      {selected ? (
        <FunctionEditor
          diagnostics={scriptDiagnostics(preview, functions.indexOf(selected), selected)}
          func={selected}
          functions={functions}
          key={selected.name}
          onCreateCanary={() => createCanary(selected)}
          onPromote={() => promote(selected)}
          onRemove={() => removeFunction(selected.name)}
          onUpdate={updateSelected}
        />
      ) : null}

      <section className="settings-card" aria-labelledby="waf-edge-preview-title">
        <h3 id="waf-edge-preview-title">编译预览</h3>
        <p className="settings-note">
          每次修改后自动对启用的函数逐个执行原生脚本编译（与发布时的校验一致）。
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
              <span className={`status-chip status-chip-${preview.valid ? 'ready' : 'pending'}`}>
                {preview.valid ? '草稿有效' : `${preview.issues.length} 个问题`}
              </span>{' '}
              {preview.functions.length > 0
                ? `${preview.functions.length} 个启用函数待发布`
                : '没有启用的手写函数（发布时索引仅含 WAF 生成函数）'}
            </p>
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
            {preview.functions.map((fn) => (
              <p key={fn.name}>
                <span className={`status-chip status-chip-${fn.scriptValid ? 'ready' : 'pending'}`}>
                  {fn.scriptValid ? '脚本可编译' : '脚本错误'}
                </span>{' '}
                <strong>{fn.name}</strong>{' '}
                <small>
                  {fn.phase === 'request_body' ? '请求体' : '请求头'} · sha256{' '}
                  {fn.scriptSha256.slice(0, 12)}…
                </small>
                {fn.errors.length > 0 ? (
                  <ul className="route-issues">
                    {fn.errors.map((error, errorIndex) => (
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
              </p>
            ))}
          </div>
        )}
      </section>

      <LiveEdgeFunctionsSnapshot />
    </section>
  )
}

interface FunctionEditorProps {
  func: WafEdgeFunctionDraftFunction
  functions: readonly WafEdgeFunctionDraftFunction[]
  diagnostics: readonly YamlCodeEditorDiagnostic[]
  onUpdate(next: WafEdgeFunctionDraftFunction): void
  onRemove(): void
  onCreateCanary(): void
  onPromote(): void
}

function FunctionEditor({
  func,
  functions,
  diagnostics,
  onUpdate,
  onRemove,
  onCreateCanary,
  onPromote,
}: FunctionEditorProps) {
  const title = `函数 ${func.name}`
  const phase = func.phase ?? 'request_head'
  const replaceable = functions.filter(
    (fn) =>
      fn.name !== func.name &&
      (fn.phase ?? 'request_head') === phase &&
      fn.replaces === undefined &&
      !functions.some((other) => other.name !== func.name && other.replaces === fn.name),
  )
  // Keep a now-invalid target selectable so the validator's issue stays visible
  // instead of the field silently reading as "none".
  const replaceOptions =
    func.replaces === undefined || replaceable.some((fn) => fn.name === func.replaces)
      ? replaceable.map((fn) => fn.name)
      : [func.replaces, ...replaceable.map((fn) => fn.name)]
  const canaryBlocker = func.replaces
    ? '灰度中的函数不能再创建灰度版本'
    : functions.some((fn) => fn.replaces === func.name)
      ? '本函数已有灰度版本'
      : functions.length >= maxFunctions
        ? `函数数量已达上限 ${maxFunctions}`
        : null
  const promoteBlocker = func.replaces
    ? promotionBlocker(
        func,
        functions.find((fn) => fn.name === func.replaces),
      )
    : null

  return (
    <section className="route-card" aria-labelledby={`waf-fn-${func.name}-title`}>
      <header className="route-card-header waf-function-header">
        <div>
          <strong id={`waf-fn-${func.name}-title`}>{func.name}</strong>
          <small>{func.description || '手写函数'}</small>
        </div>
        <div className="route-card-actions">
          <label>
            <input
              checked={func.enabled}
              onChange={(event) => onUpdate({ ...func, enabled: event.target.checked })}
              type="checkbox"
            />
            启用
          </label>
          <button
            className="button-secondary"
            disabled={canaryBlocker !== null}
            onClick={onCreateCanary}
            title={canaryBlocker ?? undefined}
            type="button"
          >
            创建灰度版本
          </button>
          {func.replaces ? (
            <button
              className="button-secondary"
              disabled={promoteBlocker !== null}
              onClick={onPromote}
              title={promoteBlocker ?? `移除 ${func.replaces}，本函数转为普通全量函数`}
              type="button"
            >
              转正
            </button>
          ) : null}
          <button className="button-danger" onClick={onRemove} type="button">
            移除函数
          </button>
        </div>
      </header>

      <div className="waf-rule-grid">
        <label>
          函数名（Data ID 后缀）
          <input
            aria-label={`${title} 函数名`}
            maxLength={64}
            value={func.name}
            onChange={(event) => onUpdate({ ...func, name: event.target.value })}
          />
        </label>
        <label>
          描述
          <input
            aria-label={`${title} 描述`}
            maxLength={255}
            placeholder="用途说明（审计可见）"
            value={func.description ?? ''}
            onChange={(event) => {
              const value = event.target.value
              onUpdate({ ...func, description: value === '' ? undefined : value })
            }}
          />
        </label>
        <label>
          执行阶段
          <select
            aria-label={`${title} 执行阶段`}
            value={func.phase ?? 'request_head'}
            onChange={(event) =>
              onUpdate({
                ...func,
                phase: event.target.value === 'request_body' ? 'request_body' : undefined,
              })
            }
          >
            <option value="request_head">请求头（路由匹配前）</option>
            <option value="request_body">请求体（仅 PROXY 路由 opt-in）</option>
          </select>
        </label>
      </div>
      {func.phase === 'request_body' ? (
        <p className="settings-note">
          请求体函数只在标记 edge_function_body 的 PROXY 路由上运行：控制台校验通过的白名单
          content-type（json/表单/xml/文本）且请求体不超过限额时缓冲重放；$req.body
          仅在该阶段可引用。
        </p>
      ) : null}

      <div className="waf-match-editor">
        <Suspense fallback={<p role="status">正在加载脚本编辑器…</p>}>
          <YamlCodeEditor
            ariaLabel={`${title} 脚本`}
            diagnostics={diagnostics}
            language="javascript"
            onChange={(value) => onUpdate({ ...func, script: value })}
            onSave={() => undefined}
            value={func.script}
          />
        </Suspense>
      </div>

      <div className="waf-function-mounts">
        <strong>挂载</strong>
        <p className="settings-note">
          include/exclude 为 Project 主域名通配（如 loan-*），优先级越小越先执行；采样比例
          0-10000，默认 10000（全量），按客户端 IP 粘性采样。
        </p>
        <div className="waf-rule-grid">
          <label>
            include（逗号分隔）
            <input
              aria-label={`${title} include`}
              value={func.projects.include.join(', ')}
              onChange={(event) =>
                onUpdate({
                  ...func,
                  projects: { ...func.projects, include: csvToList(event.target.value) },
                })
              }
            />
          </label>
          <label>
            exclude（逗号分隔）
            <input
              aria-label={`${title} exclude`}
              value={(func.projects.exclude ?? []).join(', ')}
              onChange={(event) =>
                onUpdate({
                  ...func,
                  projects: { ...func.projects, exclude: csvToList(event.target.value) },
                })
              }
            />
          </label>
          <label>
            优先级
            <input
              aria-label={`${title} 优先级`}
              inputMode="numeric"
              max={1000000}
              min={0}
              type="number"
              value={func.priority ?? 0}
              onChange={(event) => onUpdate({ ...func, priority: Number(event.target.value) })}
            />
          </label>
          <label>
            采样比例（‱）
            <input
              aria-label={`${title} 采样比例`}
              inputMode="numeric"
              max={fullRatio}
              min={0}
              type="number"
              value={func.ratio ?? fullRatio}
              onChange={(event) => onUpdate({ ...func, ratio: Number(event.target.value) })}
            />
          </label>
          <label>
            取代函数
            <select
              aria-label={`${title} 取代函数`}
              value={func.replaces ?? ''}
              onChange={(event) => onUpdate({ ...func, replaces: event.target.value || undefined })}
            >
              <option value="">无（未命中时不执行）</option>
              {replaceOptions.map((name) => (
                <option key={name} value={name}>
                  {name}
                </option>
              ))}
            </select>
          </label>
        </div>
        {func.replaces ? (
          <p className="settings-note">
            在两个函数都挂载的项目上，采样比例内的客户端只跑本函数，其余只跑 {func.replaces}
            ，绝不双跑；{func.replaces}{' '}
            未挂载的项目未命中时不执行。这不是异常兜底：本函数报错不会回退到 {func.replaces}。
          </p>
        ) : null}
      </div>
    </section>
  )
}

// Live rnacos snapshot (§4.4): exactly what the data plane currently serves —
// read-only, collapsed below the authoring surface.
function LiveEdgeFunctionsSnapshot() {
  const [view, setView] = useState<WafEdgeFunctionsView | null>(null)
  const [loading, setLoading] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const load = useCallback(async (signal?: AbortSignal): Promise<void> => {
    setLoading(true)
    try {
      setView(await fetchWafEdgeFunctions(signal))
      setErrorMessage(null)
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    void load(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载边缘函数快照失败')
      }
    })
    return () => controller.abort()
  }, [load])

  return (
    <details className="wire-preview live-edge-functions">
      <summary>rnacos 实时快照（只读）</summary>
      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>快照加载失败</strong>
          <span>{errorMessage}</span>
        </div>
      ) : loading && view === null ? (
        <p role="status">正在读取 rnacos 中的边缘函数…</p>
      ) : view === null ? null : !view.index.exists ? (
        <p className="settings-note">rnacos 中尚无函数索引（未发布过边缘函数）。</p>
      ) : (
        <>
          <div className="project-status-row">
            <span>索引版本：{view.index.version ?? '未知'}</span>
            <span>函数数量：{view.index.functions.length}</span>
            <span>sha256：{view.index.sha256 ? `${view.index.sha256.slice(0, 16)}…` : '未知'}</span>
          </div>
          {view.index.parseError ? (
            <div className="error-banner" role="alert">
              <strong>索引解析失败</strong>
              <span>{view.index.parseError}</span>
            </div>
          ) : null}
          <div className="release-resources">
            {view.index.functions.map((entry) => (
              <div key={entry.name}>
                <span>
                  <strong>{entry.name}</strong>
                  <small>
                    {entry.sha256 ? `脚本 ${entry.sha256.slice(0, 16)}…` : '无脚本摘要'} ·{' '}
                    {entry.phase === 'request_body'
                      ? '请求体阶段'
                      : entry.phase === 'request_head'
                        ? '请求头阶段'
                        : '未声明阶段（默认请求头）'}{' '}
                    ·{' '}
                    {`挂载 include=${entry.projects.include.join(',') || '*'} exclude=${
                      entry.projects.exclude.join(',') || '-'
                    } 优先级 ${entry.priority ?? '未知'}${
                      entry.ratio !== null && entry.ratio !== fullRatio
                        ? ` 采样 ${entry.ratio}/10000`
                        : ''
                    }${entry.replaces ? ` 取代 ${entry.replaces}` : ''}`}
                  </small>
                </span>
              </div>
            ))}
          </div>
          {view.scripts.map((script) => (
            <details className="wire-preview" key={script.name}>
              <summary>脚本字节：{script.name}</summary>
              {script.exists && script.content !== null ? (
                <pre>{script.content}</pre>
              ) : (
                <p className="settings-note">索引引用了该函数，但 rnacos 中没有对应脚本字节。</p>
              )}
            </details>
          ))}
        </>
      )}
    </details>
  )
}
