import { lazy, Suspense, useEffect, useMemo, useState, type FormEvent } from 'react'

import type {
  ApiConnectionState,
  ConfigurationVersionDetail,
  ConfigurationVersionSummary,
  HealthResponse,
  ProjectReleaseView,
  ProjectRoutesModel,
  ProjectRoutesValidationView,
  ProjectView,
  RouteItemModel,
  RouteValidationIssue,
  SystemStatusResponse,
} from '../api/types'
import {
  analyzeRouteSource,
  createRouteItem,
  duplicateRouteItem,
  type RouteTemplate,
} from '../routes/model'

const YamlCodeEditor = lazy(async () => {
  const module = await import('../components/YamlCodeEditor')
  return { default: module.YamlCodeEditor }
})

interface ProjectsPageProps {
  apiState: ApiConnectionState
  health: HealthResponse | null
  systemStatus: SystemStatusResponse | null
  projects: readonly ProjectView[]
  selectedProjectId: string | null
  model: ProjectRoutesModel
  versions: readonly ConfigurationVersionSummary[]
  currentVersionId: string | null
  configurationLockVersion: string
  releases: readonly ProjectReleaseView[]
  previewVersion: ConfigurationVersionDetail | null
  routeLoading: boolean
  hasUnsavedChanges: boolean
  saving: boolean
  publishing: boolean
  validating: boolean
  validation: ProjectRoutesValidationView | null
  errorMessage: string | null
  onSelectProject(id: string): void
  onCreateProject(domain: string): Promise<void>
  onModelChange(model: ProjectRoutesModel): void
  onSaveRoutes(changeSummary: string): Promise<void>
  onRestoreVersion(versionId: string, number: number): Promise<void>
  onPublishVersion(versionId: string, title: string, description: string): Promise<void>
  onViewVersion(versionId: string): Promise<void>
  onClosePreview(): void
  onValidateRoutes(): Promise<void>
}

function capabilityStatus(capability: { status: string; detail: string } | undefined): {
  label: string
  tone: 'ready' | 'pending' | 'unknown'
} {
  if (capability?.status === 'ready') return { label: '可用', tone: 'ready' }
  if (capability?.status === 'unavailable') return { label: '不可用', tone: 'pending' }
  return { label: '未配置', tone: 'unknown' }
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

export function ProjectsPage(props: ProjectsPageProps) {
  const {
    apiState,
    health,
    systemStatus,
    projects,
    selectedProjectId,
    model,
    versions,
    currentVersionId,
    configurationLockVersion,
    releases,
    previewVersion,
    routeLoading,
    hasUnsavedChanges,
    saving,
    publishing,
    validating,
    validation,
    errorMessage,
    onSelectProject,
    onCreateProject,
    onModelChange,
    onSaveRoutes,
    onRestoreVersion,
    onPublishVersion,
    onViewVersion,
    onClosePreview,
    onValidateRoutes,
  } = props
  const [domain, setDomain] = useState('')
  const [query, setQuery] = useState('')
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(new Set())
  const [actionError, setActionError] = useState<string | null>(null)
  const [saveDialogOpen, setSaveDialogOpen] = useState(false)
  const [changeSummary, setChangeSummary] = useState('')
  const [publishDialogOpen, setPublishDialogOpen] = useState(false)
  const [publishVersionId, setPublishVersionId] = useState('')
  const [releaseTitle, setReleaseTitle] = useState('')
  const [releaseDescription, setReleaseDescription] = useState('')
  const selectedProject = projects.find((item) => item.id === selectedProjectId) ?? null
  const filteredProjects = useMemo(() => {
    const normalized = query.trim().toLowerCase()
    return normalized ? projects.filter((project) => project.domain.includes(normalized)) : projects
  }, [projects, query])
  const validatorCapability = capabilityStatus(systemStatus?.dependencies.nativeValidator)
  const publicationCapability = capabilityStatus(systemStatus?.dependencies.publicationWorker)
  const currentVersion = versions.find((version) => version.id === currentVersionId) ?? null
  const localIssueCount = model.routes.reduce(
    (total, route) => total + analyzeRouteSource(route).issues.length,
    0,
  )
  const projectIssues =
    validation?.issues.filter(
      (issue) => !model.routes.some((route) => route.id === issue.routeId),
    ) ?? []

  useEffect(() => {
    setExpanded((current) => {
      if (model.routes.some((route) => current.has(route.id))) return current
      return model.routes[0] ? new Set([model.routes[0].id]) : new Set()
    })
  }, [model.routes, selectedProjectId])

  useEffect(() => {
    setSaveDialogOpen(false)
    setPublishDialogOpen(false)
    setChangeSummary('')
    setPublishVersionId('')
  }, [selectedProjectId])

  const updateRoutes = (routes: readonly RouteItemModel[]): void => {
    onModelChange({ ...model, routes })
  }

  const submitProject = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setActionError(null)
    try {
      await onCreateProject(domain)
      setDomain('')
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '创建域名失败')
    }
  }

  const runAction = async (action: () => Promise<void>): Promise<void> => {
    setActionError(null)
    try {
      await action()
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '操作失败')
    }
  }

  const submitVersion = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    await runAction(async () => {
      await onSaveRoutes(changeSummary)
      setSaveDialogOpen(false)
      setChangeSummary('')
    })
  }

  const openPublishDialog = (version: ConfigurationVersionSummary): void => {
    onClosePreview()
    setPublishVersionId(version.id)
    setReleaseTitle(`发布 V${version.number} · ${selectedProject?.domain ?? ''}`)
    setReleaseDescription(version.changeSummary)
    setPublishDialogOpen(true)
  }

  const submitRelease = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    await runAction(async () => {
      await onPublishVersion(publishVersionId, releaseTitle, releaseDescription)
      setPublishDialogOpen(false)
    })
  }

  const addRoute = (template: RouteTemplate): void => {
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
    if (!window.confirm(`确定删除路由 ${label} 吗？保存后仍可从历史 revision 恢复。`)) return
    updateRoutes(model.routes.filter((item) => item.id !== route.id))
  }

  return (
    <div className="projects-page">
      <header className="page-header projects-header">
        <div>
          <p className="eyebrow">ROUTE-FIRST CONTROL PLANE</p>
          <h1>Projects</h1>
          <p className="page-description">
            一个 Project 对应一个域名。每条 Route 使用独立 YAML 编辑器，并按页面顺序编译发布。
          </p>
        </div>
        <div className="header-actions">
          <button
            className="button-secondary"
            disabled={!selectedProject || routeLoading || validating || saving}
            onClick={() => void runAction(onValidateRoutes)}
            type="button"
          >
            {validating ? '校验中…' : '校验配置'}
          </button>
          <button
            className="button-primary"
            disabled={!selectedProject || routeLoading || saving || !hasUnsavedChanges}
            onClick={() => setSaveDialogOpen(true)}
            type="button"
          >
            {saving ? '保存中…' : '保存为版本'}
          </button>
        </div>
      </header>

      {errorMessage || actionError ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{actionError ?? errorMessage}</span>
        </div>
      ) : null}

      <section className="capability-strip" aria-label="系统能力">
        <div>
          <span className={`connection-dot connection-dot-${apiState}`} aria-hidden="true" />
          <span>Console API</span>
          <strong>{apiState === 'online' ? `v${health?.version ?? '—'}` : '连接失败'}</strong>
        </div>
        <div>
          <span>Native Validator</span>
          <span className={`status-chip status-chip-${validatorCapability.tone}`}>
            {validatorCapability.label}
          </span>
        </div>
        <div>
          <span>rnacos 发布</span>
          <span className={`status-chip status-chip-${publicationCapability.tone}`}>
            {publicationCapability.label}
          </span>
        </div>
        <div>
          <span>实例激活</span>
          <span className="status-chip status-chip-unknown">未知</span>
        </div>
      </section>

      <div className="project-workspace">
        <aside className="project-rail" aria-label="域名项目">
          <div className="rail-heading">
            <div>
              <p className="eyebrow">DOMAIN PROJECTS</p>
              <h2>域名</h2>
            </div>
            <span>{projects.length}</span>
          </div>
          <label className="project-search">
            <span className="sr-only">搜索域名</span>
            <input
              placeholder="搜索域名"
              type="search"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
            />
          </label>
          <div className="project-list">
            {filteredProjects.map((project) => (
              <button
                className={
                  project.id === selectedProjectId ? 'project-item active' : 'project-item'
                }
                key={project.id}
                onClick={() => onSelectProject(project.id)}
                type="button"
              >
                <span className="project-domain-mark" aria-hidden="true">
                  {project.domain.charAt(0).toUpperCase()}
                </span>
                <span>
                  <strong>{project.domain}</strong>
                  <small>
                    {project.draft?.revision ? `当前 V${project.draft.revision}` : '尚无版本'} ·
                    激活未知
                  </small>
                </span>
              </button>
            ))}
            {filteredProjects.length === 0 ? (
              <p className="empty-state">
                {projects.length === 0 ? '还没有域名项目' : '没有匹配域名'}
              </p>
            ) : null}
          </div>
          <form className="project-create-form" onSubmit={(event) => void submitProject(event)}>
            <label>
              新建域名
              <input
                autoComplete="off"
                placeholder="api.example.com"
                required
                value={domain}
                onChange={(event) => setDomain(event.target.value)}
              />
            </label>
            <button className="button-primary" disabled={saving} type="submit">
              创建 Project
            </button>
          </form>
        </aside>

        <main className="route-workspace">
          {selectedProject ? (
            <>
              <section className="project-context">
                <div>
                  <p className="eyebrow">PROJECT / ROUTES</p>
                  <h2>{selectedProject.domain}</h2>
                  <div className="project-status-row">
                    <span className="status-chip status-chip-pending">
                      {hasUnsavedChanges ? '工作区有未保存修改' : '工作区已同步'}
                    </span>
                    <span>当前：{currentVersion ? `V${currentVersion.number}` : '尚无版本'}</span>
                    <span>
                      已发布：
                      {selectedProject.publishedVersion
                        ? `V${selectedProject.publishedVersion}`
                        : '尚未发布'}
                    </span>
                    <span>激活：未知</span>
                    <span>证书：动态部署未接入</span>
                  </div>
                </div>
                <div className="route-count">
                  <strong>{model.routes.length}</strong>
                  <span>Routes</span>
                </div>
              </section>

              <section className="route-toolbar" aria-label="路由操作">
                <div>
                  <button
                    className="button-secondary"
                    onClick={() => addRoute('RESPONSE')}
                    type="button"
                  >
                    + RESPONSE
                  </button>
                  <button
                    className="button-secondary"
                    onClick={() => addRoute('PROXY')}
                    type="button"
                  >
                    + PROXY
                  </button>
                </div>
                <div className="route-toolbar-status" role="status">
                  {localIssueCount > 0 ? (
                    <span className="status-chip status-chip-pending">
                      {localIssueCount} 个 YAML 问题
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
                    YAML
                  </span>
                  <h3>从第一条 Route 开始</h3>
                  <p>选择 RESPONSE 或 PROXY 模板，每条路由都会拥有独立的 YAML 编辑器。</p>
                  <div>
                    <button
                      className="button-secondary"
                      onClick={() => addRoute('RESPONSE')}
                      type="button"
                    >
                      新建 RESPONSE
                    </button>
                    <button
                      className="button-primary"
                      onClick={() => addRoute('PROXY')}
                      type="button"
                    >
                      新建 PROXY
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
                                {analysis.type ?? 'YAML'}
                              </span>
                              <strong>{analysis.path ?? '无法解析 path'}</strong>
                            </div>
                            <small>
                              {analysis.condition
                                ? `condition · ${analysis.condition}`
                                : `route · ${route.id.slice(0, 8)}`}
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
                            <button
                              className="danger"
                              onClick={() => deleteRoute(route)}
                              type="button"
                            >
                              删除
                            </button>
                          </div>
                        </header>
                        {!isCollapsed ? (
                          <>
                            <Suspense
                              fallback={<div className="editor-loading">正在加载 YAML 编辑器…</div>}
                            >
                              <YamlCodeEditor
                                ariaLabel={`路由 ${index + 1}：${analysis.type ?? 'YAML'} ${analysis.path ?? route.id}`}
                                value={route.source}
                                onChange={(source) =>
                                  updateRoutes(
                                    model.routes.map((item) =>
                                      item.id === route.id ? { ...item, source } : item,
                                    ),
                                  )
                                }
                                onSave={() => setSaveDialogOpen(true)}
                              />
                            </Suspense>
                            {issues.length > 0 ? (
                              <ul className="route-issues" aria-label="路由错误">
                                {issues.map((issue) => (
                                  <li
                                    key={`${issue.code}:${issue.path}:${issue.line}:${issue.column}`}
                                  >
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
                                YAML 语法有效 · 等待完整 Native 校验
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
                  <summary>
                    查看编译后的 rnacos JSON · {validation.wireSha256?.slice(0, 12)}
                  </summary>
                  <pre>{JSON.stringify(JSON.parse(validation.wirePreview), null, 2)}</pre>
                </details>
              ) : null}

              <div className="lifecycle-grid">
                <section className="history-panel" aria-labelledby="version-history-title">
                  <header className="panel-heading">
                    <div>
                      <p className="eyebrow">IMMUTABLE CONFIGURATION</p>
                      <h3 id="version-history-title">配置版本</h3>
                    </div>
                    <span>
                      {versions.length} 个版本 · ETag {configurationLockVersion}
                    </span>
                  </header>
                  {versions.length === 0 ? (
                    <p className="panel-empty">
                      编辑 Route 后保存，系统会创建第一个不可变版本 V1。
                    </p>
                  ) : (
                    <ol className="version-list">
                      {versions.map((version) => (
                        <li key={version.id}>
                          <div className="version-number">
                            <strong>V{version.number}</strong>
                            <span>{version.relation === 'current' ? '当前' : '历史'}</span>
                          </div>
                          <div className="version-summary">
                            <strong>{version.changeSummary}</strong>
                            <small>
                              {version.routeCount} Routes · {version.createdBy.displayName} ·{' '}
                              {new Date(version.createdAt).toLocaleString('zh-CN')}
                            </small>
                          </div>
                          <div className="version-state">
                            <span className="status-chip status-chip-unknown">
                              {version.publicationStatus === 'never'
                                ? '未发布'
                                : version.publicationStatus}
                            </span>
                          </div>
                          <div className="version-actions">
                            <button
                              className="button-secondary"
                              onClick={() => void runAction(() => onViewVersion(version.id))}
                              type="button"
                            >
                              查看
                            </button>
                            <button
                              className="button-primary"
                              disabled={publishing || publicationCapability.tone !== 'ready'}
                              onClick={() => openPublishDialog(version)}
                              type="button"
                            >
                              发布
                            </button>
                            {version.relation === 'historical' ? (
                              <button
                                className="button-secondary"
                                disabled={saving}
                                onClick={() =>
                                  void runAction(() => onRestoreVersion(version.id, version.number))
                                }
                                type="button"
                              >
                                恢复为新版本
                              </button>
                            ) : null}
                          </div>
                        </li>
                      ))}
                    </ol>
                  )}
                </section>

                <section className="history-panel" aria-labelledby="release-history-title">
                  <header className="panel-heading">
                    <div>
                      <p className="eyebrow">RNACOS PUBLICATION</p>
                      <h3 id="release-history-title">Release 记录</h3>
                    </div>
                    <span>激活状态始终单独统计</span>
                  </header>
                  {releases.length === 0 ? (
                    <p className="panel-empty">尚未创建 Release。工作区内容不能直接发布。</p>
                  ) : (
                    <ol className="release-list">
                      {releases.map((release) => (
                        <li key={release.id}>
                          <div>
                            <strong>R{release.sequence}</strong>
                            <span>V{release.sourceConfigurationVersion.number}</span>
                          </div>
                          <div>
                            <strong>{release.title}</strong>
                            <small>
                              wire v{release.allocatedWireVersion} ·{' '}
                              {new Date(release.createdAt).toLocaleString('zh-CN')}
                            </small>
                          </div>
                          <div className="release-state-stack">
                            <span className="status-chip status-chip-unknown">
                              {release.status}
                            </span>
                            <small>实例激活：未知</small>
                          </div>
                        </li>
                      ))}
                    </ol>
                  )}
                </section>
              </div>
            </>
          ) : (
            <div className="route-empty-state">
              <h3>选择或创建一个域名 Project</h3>
              <p>Route、证书和发布历史都围绕域名组织，不需要先选择环境。</p>
            </div>
          )}
        </main>
      </div>

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
            <p>
              本次会创建 {currentVersion ? `V${currentVersion.number + 1}` : 'V1'}
              。保存后内容不可修改， 后续修改会继续创建新版本。
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
              <button className="button-primary" disabled={saving} type="submit">
                {saving ? '保存中…' : '创建版本'}
              </button>
            </div>
          </form>
        </div>
      ) : null}

      {publishDialogOpen ? (
        <div className="dialog-backdrop" role="presentation">
          <form
            aria-labelledby="publish-version-title"
            className="dialog-card"
            onSubmit={(event) => void submitRelease(event)}
            role="dialog"
          >
            <p className="eyebrow">CREATE & PUBLISH RELEASE</p>
            <h2 id="publish-version-title">发布配置版本</h2>
            {hasUnsavedChanges ? (
              <div className="dialog-warning">
                工作区还有未保存修改；本次只发布所选历史快照，不包含这些修改。
              </div>
            ) : null}
            <label>
              发布版本
              <select
                required
                value={publishVersionId}
                onChange={(event) => setPublishVersionId(event.target.value)}
              >
                {versions.map((version) => (
                  <option key={version.id} value={version.id}>
                    V{version.number} · {version.relation === 'current' ? '当前' : '历史'} ·{' '}
                    {version.changeSummary}
                  </option>
                ))}
              </select>
            </label>
            <label>
              Release 标题
              <input
                maxLength={255}
                required
                value={releaseTitle}
                onChange={(event) => setReleaseTitle(event.target.value)}
              />
            </label>
            <label>
              说明
              <textarea
                maxLength={4000}
                rows={3}
                value={releaseDescription}
                onChange={(event) => setReleaseDescription(event.target.value)}
              />
            </label>
            <p className="dialog-footnote">
              创建 Release 时会分配新的 native wire version，并用当前 Native Validator 重新校验。
            </p>
            <div className="dialog-actions">
              <button
                className="button-secondary"
                disabled={publishing}
                onClick={() => setPublishDialogOpen(false)}
                type="button"
              >
                取消
              </button>
              <button className="button-primary" disabled={publishing} type="submit">
                {publishing ? '发布中…' : '创建并发布'}
              </button>
            </div>
          </form>
        </div>
      ) : null}

      {previewVersion ? (
        <div className="dialog-backdrop" role="presentation">
          <section
            aria-labelledby="preview-version-title"
            className="dialog-card dialog-card-wide"
            role="dialog"
          >
            <p className="eyebrow">READ-ONLY SNAPSHOT</p>
            <h2 id="preview-version-title">V{previewVersion.number} · 配置快照</h2>
            <p>{previewVersion.changeSummary}</p>
            <div className="version-preview-routes">
              {previewVersion.model.routes.map((route, index) => (
                <article key={route.id}>
                  <strong>Route {index + 1}</strong>
                  <pre>{route.source}</pre>
                </article>
              ))}
            </div>
            <div className="dialog-actions">
              <button className="button-secondary" onClick={onClosePreview} type="button">
                关闭
              </button>
              <button
                className="button-primary"
                disabled={publishing || publicationCapability.tone !== 'ready'}
                onClick={() => openPublishDialog(previewVersion)}
                type="button"
              >
                发布这个版本
              </button>
            </div>
          </section>
        </div>
      ) : null}
    </div>
  )
}
