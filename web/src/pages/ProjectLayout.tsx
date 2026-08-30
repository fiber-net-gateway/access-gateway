import { useCallback, useEffect, useMemo, useState, type FormEvent } from 'react'
import {
  Link,
  NavLink,
  Outlet,
  useLocation,
  useNavigate,
  useOutletContext,
  useParams,
  useSearchParams,
} from 'react-router'

import {
  ApiClientError,
  fetchConfigurationVersion,
  fetchCurrentConfigurationVersion,
  fetchProject,
  restoreConfigurationVersion,
  saveConfigurationVersion,
  validateProjectRoutes,
} from '../api/client'
import type {
  ConfigurationVersionDetail,
  ProjectRoutesModel,
  ProjectRoutesValidationView,
  ProjectView,
} from '../api/types'
import { useConsoleContext, type ConsoleContextValue } from '../App'
import { activationLabel } from '../components/ActivationEvidencePanel'
import { CapabilityStrip } from '../components/CapabilityStrip'
import { analyzeRouteSource, initialRouteModel, validateHostAliases } from '../routes/model'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'

export interface ProjectContextValue extends ConsoleContextValue {
  project: ProjectView
  refreshProject(): Promise<void>
  workspace: ProjectRoutesModel
  setWorkspace: React.Dispatch<React.SetStateAction<ProjectRoutesModel>>
  workspaceLoading: boolean
  workspaceDirty: boolean
  currentVersionId: string | null
  currentVersionNumber: number | null
  sourceVersion: ConfigurationVersionDetail | null
  validation: ProjectRoutesValidationView | null
  setValidation: React.Dispatch<React.SetStateAction<ProjectRoutesValidationView | null>>
}

export function useProjectContext(): ProjectContextValue {
  return useOutletContext<ProjectContextValue>()
}

const projectNavigation = [
  { path: 'routes', label: 'Routes', detail: '编辑与校验' },
  { path: 'host-policy', label: 'Host Policy', detail: '域名与 HTTPS' },
  { path: 'network-policy', label: 'Network Policy', detail: 'CIDR 与访问来源' },
  { path: 'versions', label: 'Versions', detail: '不可变配置' },
  { path: 'releases', label: 'Releases', detail: 'rnacos 发布' },
  { path: 'settings', label: 'Settings', detail: '生命周期与归档' },
]

export function ProjectLayout() {
  const consoleContext = useConsoleContext()
  const { projectId } = useParams()
  const location = useLocation()
  const navigate = useNavigate()
  const [searchParams] = useSearchParams()
  const sourceVersionId = searchParams.get('sourceVersionId')
  const [project, setProject] = useState<ProjectView | null>(null)
  const [loading, setLoading] = useState(true)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [workspace, setWorkspace] = useState<ProjectRoutesModel>(initialRouteModel)
  const [savedWorkspace, setSavedWorkspace] = useState<ProjectRoutesModel>(initialRouteModel)
  const [workspaceLoading, setWorkspaceLoading] = useState(true)
  const [currentVersionId, setCurrentVersionId] = useState<string | null>(null)
  const [currentVersionNumber, setCurrentVersionNumber] = useState<number | null>(null)
  const [lockVersion, setLockVersion] = useState('0')
  const [sourceVersion, setSourceVersion] = useState<ConfigurationVersionDetail | null>(null)
  const [validation, setValidation] = useState<ProjectRoutesValidationView | null>(null)
  const [validating, setValidating] = useState(false)
  const [saving, setSaving] = useState(false)
  const [saveDialogOpen, setSaveDialogOpen] = useState(false)
  const [changeSummary, setChangeSummary] = useState('')
  const [workspaceError, setWorkspaceError] = useState<string | null>(null)
  const workspaceDirty =
    !workspaceLoading && JSON.stringify(workspace) !== JSON.stringify(savedWorkspace)
  const changedSectionCount = [
    JSON.stringify(workspace.routes) !== JSON.stringify(savedWorkspace.routes),
    JSON.stringify(workspace.hostAliases) !== JSON.stringify(savedWorkspace.hostAliases) ||
      workspace.networkPolicy.httpsRedirect !== savedWorkspace.networkPolicy.httpsRedirect,
    JSON.stringify({
      source: workspace.networkPolicy.source,
      allowedCidrs: workspace.networkPolicy.allowedCidrs,
      deniedCidrs: workspace.networkPolicy.deniedCidrs,
    }) !==
      JSON.stringify({
        source: savedWorkspace.networkPolicy.source,
        allowedCidrs: savedWorkspace.networkPolicy.allowedCidrs,
        deniedCidrs: savedWorkspace.networkPolicy.deniedCidrs,
      }),
  ].filter(Boolean).length
  const workspaceHasLocalIssues = project
    ? workspace.routes.some((route) => analyzeRouteSource(route).issues.length > 0) ||
      validateHostAliases(workspace.hostAliases, project.domain).validationIssues.length > 0
    : false

  const loadProject = useCallback(
    async (signal?: AbortSignal): Promise<void> => {
      if (!projectId) return
      setLoading(true)
      try {
        setProject(await fetchProject(projectId, signal))
        setErrorMessage(null)
      } finally {
        setLoading(false)
      }
    },
    [projectId],
  )

  useEffect(() => {
    const controller = new AbortController()
    void loadProject(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 Project 失败')
      }
    })
    return () => controller.abort()
  }, [loadProject])

  useEffect(() => {
    if (!projectId) return
    const controller = new AbortController()
    setWorkspaceLoading(true)
    void Promise.all([
      fetchCurrentConfigurationVersion(projectId, controller.signal),
      sourceVersionId
        ? fetchConfigurationVersion(projectId, sourceVersionId, controller.signal)
        : Promise.resolve(null),
    ])
      .then(([current, source]) => {
        const currentModel = current?.version.model ?? initialRouteModel()
        const historicalSource = source && source.id !== current?.version.id ? source : null
        setWorkspace(historicalSource?.model ?? currentModel)
        setSavedWorkspace(currentModel)
        setCurrentVersionId(current?.version.id ?? null)
        setCurrentVersionNumber(current?.version.number ?? null)
        setLockVersion(current?.lockVersion ?? '0')
        setSourceVersion(historicalSource)
        setValidation(null)
        setWorkspaceError(null)
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted)
          setWorkspaceError(error instanceof Error ? error.message : '加载 Project 工作区失败')
      })
      .finally(() => {
        if (!controller.signal.aborted) setWorkspaceLoading(false)
      })
    return () => controller.abort()
  }, [projectId, sourceVersionId])

  const configurationRoot = projectId ? `/projects/${projectId}/` : ''
  const configurationTabs = useMemo(() => new Set(['routes', 'host-policy', 'network-policy']), [])
  useUnsavedChangesGuard(
    workspaceDirty,
    '当前 Project 工作区尚未保存为版本，确定放弃吗？',
    (_current, next) =>
      configurationTabs.has(next.slice(configurationRoot.length).split('/')[0] ?? ''),
  )

  const activeProjectTab = location.pathname.slice(configurationRoot.length).split('/')[0] ?? ''
  const isConfigurationTab = configurationTabs.has(activeProjectTab)

  const discardWorkspace = async (): Promise<void> => {
    if (!window.confirm('放弃当前 Project Working Copy 中的全部未保存修改吗？')) return
    setWorkspace(savedWorkspace)
    setValidation(null)
    setWorkspaceError(null)
    if (sourceVersion) {
      await navigate({ pathname: location.pathname, search: '' }, { replace: true })
    }
  }

  const validateWorkspace = async (): Promise<void> => {
    if (!projectId) return
    setValidating(true)
    setWorkspaceError(null)
    try {
      setValidation(await validateProjectRoutes(projectId, workspace))
    } catch (error) {
      setWorkspaceError(error instanceof Error ? error.message : '校验 Project 配置失败')
    } finally {
      setValidating(false)
    }
  }

  const submitWorkspace = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    if (!projectId) return
    setSaving(true)
    setWorkspaceError(null)
    try {
      const checked = await validateProjectRoutes(projectId, workspace)
      setValidation(checked)
      if (!checked.valid)
        throw new Error(`完整 Project 配置有 ${checked.issues.length} 个问题，请修复后再保存。`)
      const saved =
        sourceVersion && currentVersionId
          ? await restoreConfigurationVersion(
              projectId,
              sourceVersion.id,
              currentVersionId,
              lockVersion,
              changeSummary,
              workspace,
            )
          : await saveConfigurationVersion(
              projectId,
              lockVersion,
              currentVersionId,
              changeSummary,
              workspace,
            )
      setWorkspace(saved.version.model)
      setSavedWorkspace(saved.version.model)
      setCurrentVersionId(saved.version.id)
      setCurrentVersionNumber(saved.version.number)
      setLockVersion(saved.lockVersion)
      setSourceVersion(null)
      setSaveDialogOpen(false)
      setChangeSummary('')
      await navigate({ pathname: location.pathname, search: '' }, { replace: true })
      await refreshProject()
    } catch (error) {
      setWorkspaceError(
        error instanceof ApiClientError && error.code === 'CONFIG_VERSION_CONFLICT'
          ? '当前配置已被其他用户更新。本地修改已保留，请重新加载并比较最新版本。'
          : error instanceof Error
            ? error.message
            : '保存 Project 配置失败',
      )
    } finally {
      setSaving(false)
    }
  }

  const refreshProject = async (): Promise<void> => loadProject()

  if (loading && !project) {
    return <div className="route-empty-state page-loading-state">正在加载 Project…</div>
  }

  if (!project || !projectId) {
    return (
      <div className="projects-page">
        <div className="error-banner" role="alert">
          <strong>无法打开 Project</strong>
          <span>{errorMessage ?? 'Project 不存在或当前用户无权访问'}</span>
        </div>
        <Link className="button-secondary inline-button-link" to="/projects">
          返回 Projects
        </Link>
      </div>
    )
  }

  return (
    <div className="projects-page project-detail-page">
      <div className="project-breadcrumbs" aria-label="面包屑">
        <Link to="/projects">Projects</Link>
        <span aria-hidden="true">/</span>
        <span>{project.domain}</span>
      </div>

      <header className="project-detail-header">
        <div>
          <p className="eyebrow">DOMAIN PROJECT</p>
          <h1>{project.domain}</h1>
          <div className="project-status-row">
            <span>生命周期：{project.status === 'active' ? 'Active' : '下线处理中'}</span>
            <span>
              当前配置：{project.draft?.revision ? `V${project.draft.revision}` : '尚无版本'}
            </span>
            <span>
              rnacos 已发布：
              {project.publishedVersion ? `V${project.publishedVersion}` : '尚未发布'}
            </span>
            <span>实例激活：{activationLabel(project.activationStatus)}</span>
          </div>
        </div>
        <Link className="button-secondary inline-button-link" to="/projects">
          切换 Project
        </Link>
      </header>

      {consoleContext.statusError || errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>部分信息加载失败</strong>
          <span>{errorMessage ?? consoleContext.statusError}</span>
        </div>
      ) : null}

      <CapabilityStrip
        apiState={consoleContext.apiState}
        health={consoleContext.health}
        systemStatus={consoleContext.systemStatus}
      />

      <nav className="project-tabs" aria-label="Project 功能">
        {projectNavigation.map((item) => (
          <NavLink
            className={({ isActive }) => `project-tab${isActive ? ' project-tab-active' : ''}`}
            key={item.path}
            to={
              sourceVersionId && configurationTabs.has(item.path)
                ? {
                    pathname: item.path,
                    search: `?sourceVersionId=${encodeURIComponent(sourceVersionId)}`,
                  }
                : item.path
            }
          >
            <strong>{item.label}</strong>
            <small>{item.detail}</small>
          </NavLink>
        ))}
      </nav>

      {isConfigurationTab ? (
        <section className="project-workspace-bar" aria-label="Project 配置工作区">
          <div>
            <strong>Project Working Copy</strong>
            <span>
              {sourceVersion
                ? `历史 V${sourceVersion.number} 编辑副本`
                : currentVersionNumber
                  ? `基线 V${currentVersionNumber}`
                  : '尚无版本'}
            </span>
            <span className={`status-chip status-chip-${workspaceDirty ? 'pending' : 'ready'}`}>
              {workspaceDirty ? `${changedSectionCount} 个配置区域已修改` : '工作区已同步'}
            </span>
          </div>
          <div className="header-actions">
            <button
              className="button-secondary"
              disabled={workspaceLoading || saving || !workspaceDirty}
              onClick={() => void discardWorkspace()}
              type="button"
            >
              放弃修改
            </button>
            <button
              className="button-secondary"
              disabled={workspaceLoading || validating || saving}
              onClick={() => void validateWorkspace()}
              type="button"
            >
              {validating ? '校验中…' : '校验完整配置'}
            </button>
            <button
              className="button-primary"
              disabled={workspaceLoading || saving || !workspaceDirty || workspaceHasLocalIssues}
              onClick={() => setSaveDialogOpen(true)}
              title={workspaceHasLocalIssues ? '请先修复配置问题' : undefined}
              type="button"
            >
              保存为版本
            </button>
          </div>
        </section>
      ) : null}

      {isConfigurationTab && workspaceError ? (
        <div className="error-banner" role="alert">
          <strong>Project 工作区操作未完成</strong>
          <span>{workspaceError}</span>
        </div>
      ) : null}

      <div className="project-page-content">
        <Outlet
          context={{
            ...consoleContext,
            project,
            refreshProject,
            workspace,
            setWorkspace,
            workspaceLoading,
            workspaceDirty,
            currentVersionId,
            currentVersionNumber,
            sourceVersion,
            validation,
            setValidation,
          }}
        />
      </div>

      {saveDialogOpen ? (
        <div className="dialog-backdrop" role="presentation">
          <form
            aria-labelledby="save-version-title"
            className="dialog-card"
            onSubmit={(event) => void submitWorkspace(event)}
            role="dialog"
          >
            <p className="eyebrow">CREATE PROJECT VERSION</p>
            <h2 id="save-version-title">保存完整 Project 配置</h2>
            <p>
              将 Routes、Host Policy 和 Network Policy 一起保存为
              {currentVersionNumber ? ` V${currentVersionNumber + 1}` : ' V1'}；保存前会执行完整
              Native 校验。
            </p>
            <label>
              变更摘要
              <input
                autoFocus
                maxLength={200}
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
                {saving
                  ? '保存中…'
                  : `保存为 ${currentVersionNumber ? `V${currentVersionNumber + 1}` : 'V1'}`}
              </button>
            </div>
          </form>
        </div>
      ) : null}
    </div>
  )
}
