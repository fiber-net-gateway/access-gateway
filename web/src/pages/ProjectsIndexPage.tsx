import { useCallback, useEffect, useMemo, useState, type FormEvent } from 'react'
import { Link, useNavigate } from 'react-router'

import { createProject, fetchProjects } from '../api/client'
import type { ProjectView } from '../api/types'
import { useConsoleContext } from '../App'
import { activationLabel } from '../components/ActivationEvidencePanel'
import { CapabilityStrip } from '../components/CapabilityStrip'

export function ProjectsIndexPage() {
  const { apiState, health, systemStatus, statusError } = useConsoleContext()
  const navigate = useNavigate()
  const [projects, setProjects] = useState<readonly ProjectView[]>([])
  const [loading, setLoading] = useState(true)
  const [creating, setCreating] = useState(false)
  const [domain, setDomain] = useState('')
  const [query, setQuery] = useState('')
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const loadProjects = useCallback(async (signal?: AbortSignal): Promise<void> => {
    setLoading(true)
    try {
      setProjects(await fetchProjects(signal))
      setErrorMessage(null)
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    void loadProjects(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 Projects 失败')
      }
    })
    return () => controller.abort()
  }, [loadProjects])

  const filteredProjects = useMemo(() => {
    const normalized = query.trim().toLowerCase()
    return normalized ? projects.filter((project) => project.domain.includes(normalized)) : projects
  }, [projects, query])

  const submitProject = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setCreating(true)
    setErrorMessage(null)
    try {
      const created = await createProject(domain)
      setDomain('')
      await navigate(`/projects/${created.id}/routes`)
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '创建 Project 失败')
    } finally {
      setCreating(false)
    }
  }

  return (
    <div className="projects-page projects-index-page">
      <header className="page-header projects-header">
        <div>
          <p className="eyebrow">ROUTE-FIRST CONTROL PLANE</p>
          <h1>Projects</h1>
          <p className="page-description">
            每个 Project 对应一个域名，并拥有独立的 Route、配置版本和发布历史。
          </p>
        </div>
      </header>

      {statusError || errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage ?? statusError}</span>
        </div>
      ) : null}

      <CapabilityStrip apiState={apiState} health={health} systemStatus={systemStatus} />

      <section className="project-index-panel" aria-labelledby="project-list-title">
        <header className="project-index-toolbar">
          <div>
            <p className="eyebrow">DOMAIN PROJECTS</p>
            <h2 id="project-list-title">域名项目</h2>
          </div>
          <label className="project-search project-index-search">
            <span className="sr-only">搜索域名</span>
            <input
              placeholder="搜索域名"
              type="search"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
            />
          </label>
        </header>

        {loading ? (
          <div className="route-empty-state">正在加载 Projects…</div>
        ) : filteredProjects.length > 0 ? (
          <div className="project-index-grid">
            {filteredProjects.map((project) => (
              <Link
                className="project-index-card"
                key={project.id}
                to={`${project.id}/${project.status === 'decommissioning' ? 'settings' : 'routes'}`}
              >
                <span className="project-domain-mark" aria-hidden="true">
                  {project.domain.charAt(0).toUpperCase()}
                </span>
                <span className="project-index-card-content">
                  <strong>{project.domain}</strong>
                  <small>
                    当前配置：{project.draft?.revision ? `V${project.draft.revision}` : '尚无版本'}
                  </small>
                  <span className="project-card-statuses">
                    <span>{project.status === 'active' ? 'Active' : '下线处理中'}</span>
                    <span>
                      {project.publishedVersion
                        ? `已发布 V${project.publishedVersion}`
                        : '尚未发布'}
                    </span>
                    <span>实例激活：{activationLabel(project.activationStatus)}</span>
                  </span>
                </span>
                <span className="project-card-arrow" aria-hidden="true">
                  →
                </span>
              </Link>
            ))}
          </div>
        ) : (
          <div className="route-empty-state">
            <h3>{projects.length === 0 ? '还没有域名 Project' : '没有匹配域名'}</h3>
            <p>
              {projects.length === 0
                ? '创建第一个 Project 后进入独立 Route 工作区。'
                : '尝试其他搜索条件。'}
            </p>
          </div>
        )}

        <form className="project-index-create-form" onSubmit={(event) => void submitProject(event)}>
          <label>
            新建域名 Project
            <input
              autoComplete="off"
              placeholder="api.example.com"
              required
              value={domain}
              onChange={(event) => setDomain(event.target.value)}
            />
          </label>
          <button className="button-primary" disabled={creating} type="submit">
            {creating ? '创建中…' : '创建并进入'}
          </button>
        </form>
      </section>
    </div>
  )
}
