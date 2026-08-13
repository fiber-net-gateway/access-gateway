import { useCallback, useEffect, useState } from 'react'
import { Link, NavLink, Outlet, useOutletContext, useParams } from 'react-router'

import { fetchProject } from '../api/client'
import type { ProjectView } from '../api/types'
import { useConsoleContext, type ConsoleContextValue } from '../App'
import { CapabilityStrip } from '../components/CapabilityStrip'

export interface ProjectContextValue extends ConsoleContextValue {
  project: ProjectView
  refreshProject(): Promise<void>
}

export function useProjectContext(): ProjectContextValue {
  return useOutletContext<ProjectContextValue>()
}

const projectNavigation = [
  { path: 'routes', label: 'Routes', detail: '编辑与校验' },
  { path: 'network-policy', label: 'Network Policy', detail: 'HTTPS 与 CIDR 策略' },
  { path: 'versions', label: 'Versions', detail: '不可变配置' },
  { path: 'releases', label: 'Releases', detail: 'rnacos 发布' },
  { path: 'settings', label: 'Settings', detail: '生命周期与归档' },
]

export function ProjectLayout() {
  const consoleContext = useConsoleContext()
  const { projectId } = useParams()
  const [project, setProject] = useState<ProjectView | null>(null)
  const [loading, setLoading] = useState(true)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

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
            <span>实例激活：未知</span>
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
            to={item.path}
          >
            <strong>{item.label}</strong>
            <small>{item.detail}</small>
          </NavLink>
        ))}
      </nav>

      <div className="project-page-content">
        <Outlet context={{ ...consoleContext, project, refreshProject }} />
      </div>
    </div>
  )
}
