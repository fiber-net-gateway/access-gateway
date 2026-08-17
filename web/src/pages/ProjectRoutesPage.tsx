import { lazy, Suspense, useEffect, useMemo, useState, type FormEvent } from 'react'
import { useLocation, useNavigate, useSearchParams } from 'react-router'

import {
  ApiClientError,
  fetchConfigurationVersion,
  fetchConfigurationVersions,
  fetchCurrentConfigurationVersion,
  restoreConfigurationVersion,
  saveConfigurationVersion,
  validateProjectRoutes,
} from '../api/client'
import type {
  ConfigurationVersionDetail,
  ProjectRoutesModel,
  ProjectRoutesValidationView,
  RouteItemModel,
  RouteValidationIssue,
} from '../api/types'
import {
  analyzeRouteSource,
  createRouteItem,
  duplicateRouteItem,
  initialRouteModel,
  type RouteTemplate,
} from '../routes/model'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useProjectContext } from './ProjectLayout'

const YamlCodeEditor = lazy(async () => {
  const module = await import('../components/YamlCodeEditor')
  return { default: module.YamlCodeEditor }
})

function modelFingerprint(model: ProjectRoutesModel): string {
  return JSON.stringify(model)
}

const utf8Encoder = new TextEncoder()

function utf8Bytes(value: string): number {
  return utf8Encoder.encode(value).byteLength
}

function routeIssues(
  route: RouteItemModel,
  validation: ProjectRoutesValidationView | null,
): readonly RouteValidationIssue[] {
  const local = analyzeRouteSource(route).issues
  const remote = validation?.issues.filter((issue) => issue.routeId === route.id) ?? []
  const seen = new Set<string>()
  return [...local, ...remote].filter((issue) => {
    const key = `${issue.code}:${issue.path}:${issue.line}:${issue.column}:${issue.message}`
    if (seen.has(key)) return false
    seen.add(key)
    return true
  })
}

export function ProjectRoutesPage() {
  const { project, refreshProject, systemStatus } = useProjectContext()
  const navigate = useNavigate()
  const location = useLocation()
  const [searchParams] = useSearchParams()
  const sourceVersionId = searchParams.get('sourceVersionId')
  const [model, setModel] = useState<ProjectRoutesModel>(initialRouteModel)
  const [savedFingerprint, setSavedFingerprint] = useState(modelFingerprint(initialRouteModel()))
  const [sourceVersion, setSourceVersion] = useState<ConfigurationVersionDetail | null>(null)
  const [currentVersionId, setCurrentVersionId] = useState<string | null>(null)
  const [currentVersionNumber, setCurrentVersionNumber] = useState<number | null>(null)
  const [configurationLockVersion, setConfigurationLockVersion] = useState('0')
  const [routeLoading, setRouteLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [validating, setValidating] = useState(false)
  const [validation, setValidation] = useState<ProjectRoutesValidationView | null>(null)
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(new Set())
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [saveDialogOpen, setSaveDialogOpen] = useState(false)
  const [changeSummary, setChangeSummary] = useState('')

  const hasUnsavedChanges = !routeLoading && modelFingerprint(model) !== savedFingerprint
  const sourceHasUserChanges = sourceVersion
    ? modelFingerprint(model) !== modelFingerprint(sourceVersion.model)
    : false
  useUnsavedChangesGuard(hasUnsavedChanges)

  useEffect(() => {
    const controller = new AbortController()
    setRouteLoading(true)
    void Promise.all([
      fetchConfigurationVersions(project.id, controller.signal),
      fetchCurrentConfigurationVersion(project.id, controller.signal),
      sourceVersionId
        ? fetchConfigurationVersion(project.id, sourceVersionId, controller.signal)
        : Promise.resolve(null),
    ])
      .then(([versionList, current, requestedSource]) => {
        const currentModel = current?.version.model ?? initialRouteModel()
        const historicalSource =
          requestedSource && requestedSource.id !== versionList.currentVersionId
            ? requestedSource
            : null
        const next = historicalSource?.model ?? currentModel
        setModel(next)
        setSavedFingerprint(modelFingerprint(currentModel))
        setSourceVersion(historicalSource)
        setCurrentVersionId(versionList.currentVersionId)
        setCurrentVersionNumber(current?.version.number ?? null)
        setConfigurationLockVersion(versionList.lockVersion)
        setValidation(null)
        setErrorMessage(null)
        setSaveDialogOpen(false)
        setChangeSummary('')
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : '加载 Route 工作区失败')
        }
      })
      .finally(() => {
        if (!controller.signal.aborted) setRouteLoading(false)
      })
    return () => controller.abort()
  }, [project.id, sourceVersionId])

  useEffect(() => {
    setExpanded((current) => {
      if (model.routes.some((route) => current.has(route.id))) return current
      return model.routes[0] ? new Set([model.routes[0].id]) : new Set()
    })
  }, [model.routes])

  const localIssueCount = useMemo(
    () => model.routes.reduce((total, route) => total + analyzeRouteSource(route).issues.length, 0),
    [model.routes],
  )
  const routeLimits = systemStatus?.dependencies.nativeValidator.limits?.projectRoute ?? null
  const sourceBytes = useMemo(
    () => model.routes.reduce((total, route) => total + utf8Bytes(route.source), 0),
    [model.routes],
  )
  const upstreamTlsProfileCount = useMemo(
    () =>
      model.routes.reduce(
        (total, route) => total + Number(analyzeRouteSource(route).hasUpstreamTls),
        0,
      ),
    [model.routes],
  )
  const limitMessages = useMemo(() => {
    if (!routeLimits) return []
    const messages: string[] = []
    if (model.routes.length > routeLimits.maxRoutes) {
      messages.push(`Route 数量超过 Native 上限 ${routeLimits.maxRoutes}`)
    }
    if (sourceBytes > routeLimits.maxPayloadBytes) {
      messages.push(`Route 源码合计超过 ${routeLimits.maxPayloadBytes} UTF-8 bytes`)
    }
    if (upstreamTlsProfileCount > routeLimits.maxUpstreamTlsProfiles) {
      messages.push(`上游 TLS Profile 数量超过 Native 上限 ${routeLimits.maxUpstreamTlsProfiles}`)
    }
    for (const [index, route] of model.routes.entries()) {
      const sourceLimit =
        route.format === 'js' ? routeLimits.maxScriptBytes : routeLimits.maxPayloadBytes
      if (utf8Bytes(route.source) > sourceLimit) {
        messages.push(`Route ${index + 1} 源码超过 ${sourceLimit} UTF-8 bytes`)
      }
      if (route.format === 'js' && utf8Bytes(route.path) > routeLimits.maxPathBytes) {
        messages.push(`Route ${index + 1} Path 超过 ${routeLimits.maxPathBytes} UTF-8 bytes`)
      }
      if (
        route.format === 'js' &&
        route.method &&
        utf8Bytes(route.method) > routeLimits.maxMethodBytes
      ) {
        messages.push(`Route ${index + 1} Method 超过 ${routeLimits.maxMethodBytes} UTF-8 bytes`)
      }
    }
    return messages
  }, [model.routes, routeLimits, sourceBytes, upstreamTlsProfileCount])
  const hasLimitIssues = limitMessages.length > 0
  const hasLocalIssues = localIssueCount > 0 || hasLimitIssues
  const routeCountAtLimit = routeLimits ? model.routes.length >= routeLimits.maxRoutes : false
  const projectIssues =
    validation?.issues.filter(
      (issue) => !model.routes.some((route) => route.id === issue.routeId),
    ) ?? []

  const updateRoutes = (routes: readonly RouteItemModel[]): void => {
    setModel((current) => ({ ...current, routes }))
    setValidation(null)
    setErrorMessage(null)
  }

  const runAction = async (action: () => Promise<void>): Promise<void> => {
    setErrorMessage(null)
    try {
      await action()
    } catch (error) {
      setErrorMessage(
        error instanceof ApiClientError && error.code === 'CONFIG_VERSION_CONFLICT'
          ? '当前配置已被其他用户更新。本地编辑内容已保留；请复制必要内容后重新加载并比较最新版本。'
          : error instanceof Error
            ? error.message
            : '操作失败',
      )
    }
  }

  const openSaveDialog = (): void => {
    if (hasLimitIssues) {
      setErrorMessage(limitMessages[0] ?? '配置超过 Native 资源上限')
      return
    }
    if (hasLocalIssues) {
      setErrorMessage(`当前有 ${localIssueCount} 个 Route 问题，请修复后再保存为版本。`)
      return
    }
    if (sourceVersion && !changeSummary) {
      setChangeSummary(`基于 V${sourceVersion.number} 编辑`)
    }
    setSaveDialogOpen(true)
  }

  const submitVersion = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    await runAction(async () => {
      if (!hasUnsavedChanges) throw new Error('当前工作区没有需要保存的修改')
      if (hasLocalIssues) {
        throw new Error(
          hasLimitIssues
            ? (limitMessages[0] ?? '配置超过 Native 资源上限')
            : `当前有 ${localIssueCount} 个 Route 问题，请修复后再保存为版本。`,
        )
      }
      const saveBaseVersionId = currentVersionId
      if (sourceVersion && !saveBaseVersionId) {
        throw new Error('历史版本编辑需要一个当前配置版本作为保存基线')
      }
      setSaving(true)
      try {
        const submittedModel = model
        const submittedFingerprint = modelFingerprint(submittedModel)
        const saved =
          sourceVersion && saveBaseVersionId
            ? await restoreConfigurationVersion(
                project.id,
                sourceVersion.id,
                saveBaseVersionId,
                configurationLockVersion,
                changeSummary,
                submittedModel,
              )
            : await saveConfigurationVersion(
                project.id,
                configurationLockVersion,
                saveBaseVersionId,
                changeSummary,
                submittedModel,
              )
        setCurrentVersionId(saved.version.id)
        setCurrentVersionNumber(saved.version.number)
        setConfigurationLockVersion(saved.lockVersion)
        setSavedFingerprint(submittedFingerprint)
        setValidation(null)
        setSaveDialogOpen(false)
        setChangeSummary('')
        setSourceVersion(null)
        await navigate({ pathname: location.pathname, search: '' }, { replace: true })
        await refreshProject()
      } finally {
        setSaving(false)
      }
    })
  }

  const validateRoutes = async (): Promise<void> => {
    setValidating(true)
    try {
      setValidation(await validateProjectRoutes(project.id, model))
    } finally {
      setValidating(false)
    }
  }

  const addRoute = (template: RouteTemplate): void => {
    if (routeCountAtLimit) {
      setErrorMessage(`Route 数量已达到 Native 上限 ${routeLimits?.maxRoutes ?? ''}`)
      return
    }
    const route = createRouteItem(template)
    updateRoutes([...model.routes, route])
    setExpanded(new Set([route.id]))
  }

  const toggleRoute = (routeId: string): void => {
    setExpanded((current) => (current.has(routeId) ? new Set() : new Set([routeId])))
  }

  const moveRoute = (index: number, direction: -1 | 1): void => {
    const destination = index + direction
    if (destination < 0 || destination >= model.routes.length) return
    const routes = [...model.routes]
    const [route] = routes.splice(index, 1)
    routes.splice(destination, 0, route!)
    updateRoutes(routes)
  }

  const deleteRoute = (route: RouteItemModel): void => {
    const analysis = analyzeRouteSource(route)
    const label = analysis.path ?? route.id.slice(0, 8)
    if (!window.confirm(`确定删除路由 ${label} 吗？保存后仍可从历史版本恢复。`)) return
    updateRoutes(model.routes.filter((item) => item.id !== route.id))
  }

  const discardHistoricalSource = async (): Promise<void> => {
    if (
      hasUnsavedChanges &&
      !window.confirm(`放弃基于 V${sourceVersion?.number ?? ''} 的编辑副本，并重新加载当前版本吗？`)
    ) {
      return
    }
    await navigate({ pathname: location.pathname, search: '' }, { replace: true })
  }

  return (
    <section className="route-workspace project-subpage" aria-labelledby="routes-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT / ROUTES</p>
          <h2 id="routes-title">Routes</h2>
          <div className="project-status-row">
            <span className={`status-chip status-chip-${hasUnsavedChanges ? 'pending' : 'ready'}`}>
              {sourceVersion
                ? `历史 V${sourceVersion.number} 编辑副本${sourceHasUserChanges ? ' · 已修改' : ''}`
                : hasUnsavedChanges
                  ? '有未保存修改'
                  : '工作区已同步'}
            </span>
            <span>
              保存基线：{currentVersionNumber ? `当前 V${currentVersionNumber}` : '尚无版本'}
            </span>
            <span>{model.routes.length} Routes</span>
            {routeLimits ? (
              <span>
                源码：{sourceBytes.toLocaleString()} /{' '}
                {routeLimits.maxPayloadBytes.toLocaleString()} bytes
              </span>
            ) : null}
          </div>
        </div>
        <div className="header-actions">
          <button
            className="button-secondary"
            disabled={routeLoading || validating || saving || hasLimitIssues}
            onClick={() => void runAction(validateRoutes)}
            type="button"
          >
            {validating ? '校验中…' : '校验配置'}
          </button>
          <button
            aria-describedby={hasLocalIssues ? 'route-save-state' : undefined}
            className="button-primary"
            disabled={routeLoading || saving || !hasUnsavedChanges || hasLocalIssues}
            onClick={openSaveDialog}
            title={hasLocalIssues ? '请先修复配置问题' : undefined}
            type="button"
          >
            {saving ? '保存中…' : '保存为版本'}
          </button>
        </div>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      {limitMessages.length > 0 ? (
        <div className="project-validation-errors" role="alert">
          {limitMessages.map((message) => (
            <p key={message}>
              <strong>limit_exceeded</strong>
              <span>{message}</span>
            </p>
          ))}
        </div>
      ) : null}

      {sourceVersion ? (
        <section className="historical-source-banner" aria-label="历史版本编辑来源">
          <div>
            <p className="eyebrow">HISTORICAL EDITING SOURCE</p>
            <strong>正在基于历史 V{sourceVersion.number} 编辑</strong>
            <span>
              当前 V{currentVersionNumber ?? '—'} 保持不变；保存时只创建一个新版本
              {currentVersionNumber ? ` V${currentVersionNumber + 1}` : ''}。
            </span>
          </div>
          <button
            className="button-secondary"
            onClick={() => void discardHistoricalSource()}
            type="button"
          >
            放弃副本并返回当前版本
          </button>
        </section>
      ) : null}

      <section className="route-toolbar" aria-label="路由操作">
        <div>
          <button
            className="button-secondary"
            disabled={routeCountAtLimit}
            onClick={() => addRoute('RESPONSE')}
            type="button"
          >
            + RESPONSE
          </button>
          <button
            className="button-secondary"
            disabled={routeCountAtLimit}
            onClick={() => addRoute('PROXY')}
            type="button"
          >
            + PROXY
          </button>
          <button
            className="button-secondary"
            disabled={routeCountAtLimit}
            onClick={() => addRoute('JS')}
            type="button"
          >
            + JS
          </button>
        </div>
        <div className="route-toolbar-status" role="status">
          {hasLimitIssues ? (
            <span className="status-chip status-chip-pending" id="route-save-state">
              Native 资源上限 · {limitMessages.length} 个问题
            </span>
          ) : hasLocalIssues ? (
            <span className="status-chip status-chip-pending" id="route-save-state">
              {localIssueCount} 个 Route 问题 · 修复后才能保存
            </span>
          ) : validation?.valid ? (
            <span className="status-chip status-chip-ready">Native 校验通过</span>
          ) : validation ? (
            <span className="status-chip status-chip-pending">
              Native 校验失败 · {validation.issues.length} 个问题
            </span>
          ) : (
            <span className="status-chip status-chip-unknown">尚未校验</span>
          )}
          <span>Ctrl/Cmd + S 保存为版本</span>
        </div>
      </section>

      {projectIssues.length > 0 ? (
        <div className="project-validation-errors" role="alert">
          {projectIssues.map((issue) => (
            <p key={`${issue.code}:${issue.path}`}>
              <strong>{issue.code}</strong>
              <span>{issue.message}</span>
            </p>
          ))}
        </div>
      ) : null}

      {routeLoading ? (
        <div className="route-empty-state">正在加载路由草稿…</div>
      ) : model.routes.length === 0 ? (
        <div className="route-empty-state">
          <span className="empty-route-mark" aria-hidden="true">
            ROUTE
          </span>
          <h3>从第一条 Route 开始</h3>
          <p>选择 YAML RESPONSE/PROXY 或 JavaScript，每条路由都有独立编辑器。</p>
          <div>
            <button className="button-secondary" onClick={() => addRoute('RESPONSE')} type="button">
              新建 RESPONSE
            </button>
            <button className="button-primary" onClick={() => addRoute('PROXY')} type="button">
              新建 PROXY
            </button>
            <button className="button-secondary" onClick={() => addRoute('JS')} type="button">
              新建 JS
            </button>
          </div>
        </div>
      ) : (
        <ol className="route-list">
          {model.routes.map((route, index) => {
            const analysis = analyzeRouteSource(route)
            const issues = routeIssues(route, validation)
            const isCollapsed = !expanded.has(route.id)
            return (
              <li
                className={`route-card${issues.length > 0 ? ' route-card-invalid' : ''}`}
                key={route.id}
              >
                <header className="route-card-header">
                  <button
                    aria-label={isCollapsed ? '展开路由' : '折叠路由'}
                    className="collapse-button"
                    onClick={() => toggleRoute(route.id)}
                    type="button"
                  >
                    {isCollapsed ? '›' : '⌄'}
                  </button>
                  <span className="route-order">{String(index + 1).padStart(2, '0')}</span>
                  <div className="route-title">
                    <div>
                      <span
                        className={`route-type route-type-${(analysis.type ?? 'unknown').toLowerCase()}`}
                      >
                        {analysis.type ?? route.format.toUpperCase()}
                      </span>
                      <strong>{analysis.path ?? '无法解析 path'}</strong>
                    </div>
                    <small>
                      {analysis.method
                        ? `${analysis.method} · ${route.format.toUpperCase()}`
                        : analysis.condition
                          ? `condition · ${analysis.condition}`
                          : `ALL METHODS · ${route.id.slice(0, 8)}`}
                    </small>
                  </div>
                  <div className="route-card-actions">
                    <button
                      aria-label="上移路由"
                      disabled={index === 0}
                      onClick={() => moveRoute(index, -1)}
                      type="button"
                    >
                      ↑
                    </button>
                    <button
                      aria-label="下移路由"
                      disabled={index === model.routes.length - 1}
                      onClick={() => moveRoute(index, 1)}
                      type="button"
                    >
                      ↓
                    </button>
                    <button
                      disabled={routeCountAtLimit}
                      onClick={() =>
                        updateRoutes([
                          ...model.routes.slice(0, index + 1),
                          duplicateRouteItem(route),
                          ...model.routes.slice(index + 1),
                        ])
                      }
                      type="button"
                    >
                      复制
                    </button>
                    <button className="danger" onClick={() => deleteRoute(route)} type="button">
                      删除
                    </button>
                  </div>
                </header>
                {!isCollapsed ? (
                  <>
                    {route.format === 'js' ? (
                      <div className="script-route-match-fields">
                        <label>
                          Path pattern
                          <input
                            aria-label={`路由 ${index + 1} Path pattern`}
                            maxLength={routeLimits?.maxPathBytes}
                            required
                            value={route.path}
                            onChange={(event) =>
                              updateRoutes(
                                model.routes.map((item) =>
                                  item.id === route.id && item.format === 'js'
                                    ? { ...item, path: event.target.value }
                                    : item,
                                ),
                              )
                            }
                          />
                        </label>
                        <label>
                          Method（可选）
                          <input
                            aria-label={`路由 ${index + 1} Method`}
                            maxLength={routeLimits?.maxMethodBytes}
                            placeholder="留空匹配所有 method"
                            value={route.method ?? ''}
                            onChange={(event) =>
                              updateRoutes(
                                model.routes.map((item) => {
                                  if (item.id !== route.id || item.format !== 'js') return item
                                  const method = event.target.value
                                  if (method) return { ...item, method }
                                  return {
                                    id: item.id,
                                    format: 'js',
                                    path: item.path,
                                    source: item.source,
                                  }
                                }),
                              )
                            }
                          />
                        </label>
                      </div>
                    ) : null}
                    <Suspense
                      fallback={<div className="editor-loading">正在加载 Route 编辑器…</div>}
                    >
                      <YamlCodeEditor
                        ariaLabel={`路由 ${index + 1}：${analysis.type ?? route.format.toUpperCase()} ${analysis.path ?? route.id}`}
                        diagnostics={issues}
                        language={route.format === 'js' ? 'javascript' : 'yaml'}
                        value={route.source}
                        onChange={(source) =>
                          updateRoutes(
                            model.routes.map((item) =>
                              item.id === route.id ? { ...item, source } : item,
                            ),
                          )
                        }
                        onSave={openSaveDialog}
                      />
                    </Suspense>
                    {issues.length > 0 ? (
                      <ul aria-label="路由错误" aria-live="polite" className="route-issues">
                        {issues.map((issue) => (
                          <li key={`${issue.code}:${issue.path}:${issue.line}:${issue.column}`}>
                            <span>
                              L{issue.line}:{issue.column}
                            </span>
                            <strong>{issue.code}</strong>
                            <p>{issue.message}</p>
                          </li>
                        ))}
                      </ul>
                    ) : (
                      <div className="route-valid-hint">
                        {route.format === 'yaml' ? 'YAML 语法有效' : '外置匹配字段有效'} · 等待完整
                        Native 校验
                      </div>
                    )}
                  </>
                ) : null}
              </li>
            )
          })}
        </ol>
      )}

      {validation?.wirePreview ? (
        <details className="wire-preview">
          <summary>查看编译后的 rnacos JSON · {validation.wireSha256?.slice(0, 12)}</summary>
          <pre>{JSON.stringify(JSON.parse(validation.wirePreview), null, 2)}</pre>
        </details>
      ) : null}

      {saveDialogOpen ? (
        <div className="dialog-backdrop" role="presentation">
          <form
            aria-labelledby="save-version-title"
            className="dialog-card"
            onSubmit={(event) => void submitVersion(event)}
            role="dialog"
          >
            <p className="eyebrow">CREATE IMMUTABLE VERSION</p>
            <h2 id="save-version-title">保存为配置版本</h2>
            {sourceVersion ? (
              <div className="version-source-summary">
                <div>
                  <span>编辑来源</span>
                  <strong>历史 V{sourceVersion.number}</strong>
                </div>
                <div>
                  <span>当前基线</span>
                  <strong>{currentVersionNumber ? `V${currentVersionNumber}` : '—'}</strong>
                </div>
                <div>
                  <span>保存结果</span>
                  <strong>
                    {currentVersionNumber ? `新建 V${currentVersionNumber + 1}` : '新版本'}
                  </strong>
                </div>
              </div>
            ) : null}
            <p>
              本次会创建 {currentVersionNumber ? `V${currentVersionNumber + 1}` : 'V1'}。
              {sourceVersion
                ? `历史 V${sourceVersion.number} 和当前 V${currentVersionNumber ?? '—'} 都不会被改写。`
                : '保存后内容不可修改，后续修改会继续创建新版本。'}
            </p>
            <label>
              变更摘要
              <input
                autoFocus
                maxLength={200}
                placeholder="例如：新增 /api 代理路由"
                required
                value={changeSummary}
                onChange={(event) => setChangeSummary(event.target.value)}
              />
            </label>
            <div className="dialog-actions">
              <button
                className="button-secondary"
                disabled={saving}
                onClick={() => setSaveDialogOpen(false)}
                type="button"
              >
                取消
              </button>
              <button className="button-primary" disabled={saving || hasLimitIssues} type="submit">
                {saving
                  ? '保存中…'
                  : `保存为 ${currentVersionNumber ? `V${currentVersionNumber + 1}` : 'V1'}`}
              </button>
            </div>
          </form>
        </div>
      ) : null}
    </section>
  )
}
