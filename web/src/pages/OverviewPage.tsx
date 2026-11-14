import { useEffect, useMemo, useState } from 'react'
import { Link } from 'react-router'

import { fetchProjectReleases, fetchProjects } from '../api/client'
import type { ProjectReleaseView, ProjectView } from '../api/types'
import { useConsoleContext } from '../App'
import { activationChip, activationLabel } from '../components/ActivationEvidencePanel'
import { PageToolbar } from '../components/PageToolbar'

interface ReleaseRow {
  release: ProjectReleaseView
  project: ProjectView
}

const releaseStatusLabels: Record<string, string> = {
  draft: '草稿',
  ready: '待发布',
  queued: '排队中',
  publishing: '发布中',
  published: '已发布',
  partially_published: '部分发布',
  publish_failed: '发布失败',
  validation_failed: '校验失败',
  canceled: '已取消',
  superseded: '已取代',
  abandoned: '已放弃',
}

function releaseStatusLabel(status: string): string {
  return releaseStatusLabels[status] ?? status
}

function formatTime(value: string | null): string {
  if (!value) return '—'
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(
    date.getDate(),
  ).padStart(2, '0')} ${String(date.getHours()).padStart(2, '0')}:${String(
    date.getMinutes(),
  ).padStart(2, '0')}`
}

export function OverviewPage() {
  const { apiState, health, systemStatus, statusError } = useConsoleContext()
  const [projects, setProjects] = useState<readonly ProjectView[] | null>(null)
  const [releaseRows, setReleaseRows] = useState<readonly ReleaseRow[] | null>(null)
  const [projectsError, setProjectsError] = useState<string | null>(null)

  useEffect(() => {
    const controller = new AbortController()
    void fetchProjects(controller.signal)
      .then(async (items) => {
        setProjects(items)
        setProjectsError(null)
        // Recent releases: fetch the newest release for each project, then merge.
        const sampled = await Promise.all(
          items.slice(0, 8).map(async (project) => {
            try {
              const releases = await fetchProjectReleases(project.id, controller.signal)
              return releases[0] ?? null
            } catch {
              return null
            }
          }),
        )
        if (controller.signal.aborted) return
        setReleaseRows(
          sampled
            .filter((release): release is ProjectReleaseView => release !== null)
            .map((release) => ({
              release,
              project:
                items.find((item) => item.id === release.projectId) ??
                ({
                  domain: release.projectId,
                } as ProjectView),
            }))
            .sort((a, b) => b.release.createdAt.localeCompare(a.release.createdAt))
            .slice(0, 8),
        )
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted)
          setProjectsError(error instanceof Error ? error.message : '加载 Projects 失败')
      })
    return () => controller.abort()
  }, [])

  const stats = useMemo(() => {
    const list = projects ?? []
    return {
      total: list.length,
      published: list.filter((item) => item.publishedVersion !== null).length,
      active: list.filter((item) => item.activationStatus === 'active').length,
      unpublished: list.filter((item) => item.publishedVersion === null).length,
    }
  }, [projects])

  const dependencies = systemStatus?.dependencies

  return (
    <div className="workspace workspace-wide">
      <PageToolbar
        badges={
          <span
            className={`toolbar-chip toolbar-chip-${apiState === 'online' ? 'ready' : 'pending'}`}
          >
            {apiState === 'online' ? '数据实时' : apiState === 'loading' ? '加载中' : '离线'}
          </span>
        }
        meta={health ? <span>Console API v{health.version}</span> : null}
        title="概览"
      />

      {statusError || projectsError ? (
        <div className="error-banner" role="alert">
          <strong>部分信息加载失败</strong>
          <span>{projectsError ?? statusError}</span>
        </div>
      ) : null}

      <section className="overview-stats" aria-label="项目统计">
        <Link className="overview-stat-card" to="/projects">
          <strong>{projects === null ? '—' : stats.total}</strong>
          <span>域名项目</span>
        </Link>
        <Link className="overview-stat-card" to="/projects">
          <strong>{projects === null ? '—' : stats.published}</strong>
          <span>已发布到 rnacos</span>
        </Link>
        <Link className="overview-stat-card" to="/projects">
          <strong>{projects === null ? '—' : stats.active}</strong>
          <span>实例已激活</span>
        </Link>
        <Link className="overview-stat-card" to="/projects">
          <strong>{projects === null ? '—' : stats.unpublished}</strong>
          <span>尚未发布</span>
        </Link>
      </section>

      <div className="overview-columns">
        <section className="overview-panel" aria-labelledby="overview-health-title">
          <header>
            <h2 id="overview-health-title">系统健康</h2>
          </header>
          <dl className="overview-health-list">
            <div>
              <dt>Console API</dt>
              <dd>
                <span className={`connection-dot connection-dot-${apiState}`} aria-hidden="true" />
                {apiState === 'online' ? `在线 · v${health?.version ?? '—'}` : '离线'}
              </dd>
            </div>
            {[
              { key: 'MySQL 数据库', view: dependencies?.database },
              { key: 'Schema 迁移', view: dependencies?.schema },
              { key: '认证', view: dependencies?.authentication },
              { key: 'Native Validator', view: dependencies?.nativeValidator },
              { key: 'rnacos 发布 Worker', view: dependencies?.publicationWorker },
              { key: '激活证据收集器', view: dependencies?.activationCollector },
            ].map((item) => (
              <div key={item.key}>
                <dt>{item.key}</dt>
                <dd>
                  {item.view?.status === 'ready' ? (
                    <>
                      <span className="connection-dot connection-dot-online" aria-hidden="true" />
                      {item.view.detail || '就绪'}
                    </>
                  ) : (
                    <>
                      <span className="connection-dot connection-dot-offline" aria-hidden="true" />
                      {item.view?.detail ?? '未知'}
                    </>
                  )}
                </dd>
              </div>
            ))}
          </dl>
        </section>

        <section className="overview-panel" aria-labelledby="overview-releases-title">
          <header>
            <h2 id="overview-releases-title">最近发布</h2>
            <span className="panel-note">每个 Project 取最新一条 Release</span>
          </header>
          {releaseRows === null ? (
            <div className="route-empty-state">正在加载发布记录…</div>
          ) : releaseRows.length === 0 ? (
            <div className="route-empty-state">还没有发布记录</div>
          ) : (
            <ol className="overview-release-list">
              {releaseRows.map(({ release, project }) => (
                <li key={release.id}>
                  <Link
                    className="overview-release-row"
                    to={project.id ? `/projects/${project.id}/releases` : '/projects'}
                  >
                    <strong>{project.domain}</strong>
                    <span className="overview-release-title">{release.title}</span>
                    <span className={`status-chip ${activationChip(release.activationStatus)}`}>
                      {activationLabel(release.activationStatus)}
                    </span>
                    <span className="status-chip status-chip-neutral">
                      {releaseStatusLabel(release.status)}
                    </span>
                    <time>{formatTime(release.publishedAt ?? release.createdAt)}</time>
                  </Link>
                </li>
              ))}
            </ol>
          )}
        </section>
      </div>
    </div>
  )
}
