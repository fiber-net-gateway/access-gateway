import { useCallback, useEffect, useMemo, useState } from 'react'
import { Link, useNavigate } from 'react-router'

import { fetchProjects } from '../api/client'
import type { ProjectView } from '../api/types'
import { useConsoleContext } from '../App'
import { activationLabel } from '../components/ActivationEvidencePanel'
import { NewProjectDialog } from '../components/NewProjectDialog'
import { PageToolbar } from '../components/PageToolbar'

export function ProjectsIndexPage() {
  const { statusError } = useConsoleContext()
  const navigate = useNavigate()
  const [projects, setProjects] = useState<readonly ProjectView[]>([])
  const [loading, setLoading] = useState(true)
  const [dialogOpen, setDialogOpen] = useState(false)
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

  return (
    <div className="workspace workspace-wide">
      <PageToolbar
        actions={
          <button className="button-primary" onClick={() => setDialogOpen(true)} type="button">
            + 新建项目
          </button>
        }
        meta={
          <label className="project-index-search">
            <span className="sr-only">搜索域名</span>
            <input
              placeholder="搜索域名…"
              type="search"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
            />
          </label>
        }
        title="域名项目"
      />

      {statusError || errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage ?? statusError}</span>
        </div>
      ) : null}

      <div className="page-content">
        <section aria-label="项目列表">
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
                      当前配置：
                      {project.draft?.revision ? `V${project.draft.revision}` : '尚无版本'}
                    </small>
                    <span className="project-card-statuses">
                      <span className={project.status === 'active' ? 'chip-ready' : 'chip-pending'}>
                        {project.status === 'active' ? 'Active' : '下线处理中'}
                      </span>
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
        </section>
      </div>

      <NewProjectDialog
        onClose={() => setDialogOpen(false)}
        onCreated={(created) => {
          setDialogOpen(false)
          void navigate(`/projects/${created.id}/routes`)
        }}
        open={dialogOpen}
      />
    </div>
  )
}
