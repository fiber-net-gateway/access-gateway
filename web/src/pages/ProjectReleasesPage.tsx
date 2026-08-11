import { useCallback, useEffect, useMemo, useState } from 'react'

import { fetchProjectReleases } from '../api/client'
import type { ProjectReleaseView } from '../api/types'
import { useProjectContext } from './ProjectLayout'

const terminalReleaseStatuses = new Set([
  'published',
  'partially_published',
  'publish_failed',
  'validation_failed',
  'canceled',
  'superseded',
  'abandoned',
])

export function ProjectReleasesPage() {
  const { project } = useProjectContext()
  const [releases, setReleases] = useState<readonly ProjectReleaseView[]>([])
  const [loading, setLoading] = useState(true)
  const [refreshing, setRefreshing] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const hasActiveRelease = useMemo(
    () => releases.some((release) => !terminalReleaseStatuses.has(release.status)),
    [releases],
  )

  const loadReleases = useCallback(
    async (signal?: AbortSignal, background = false): Promise<void> => {
      if (background) setRefreshing(true)
      else setLoading(true)
      try {
        setReleases(await fetchProjectReleases(project.id, signal))
        setErrorMessage(null)
      } finally {
        setLoading(false)
        setRefreshing(false)
      }
    },
    [project.id],
  )

  useEffect(() => {
    const controller = new AbortController()
    void loadReleases(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 Release 记录失败')
      }
    })
    return () => controller.abort()
  }, [loadReleases])

  useEffect(() => {
    if (!hasActiveRelease) return
    const timer = window.setInterval(() => {
      void loadReleases(undefined, true).catch((error: unknown) => {
        setErrorMessage(error instanceof Error ? error.message : '刷新 Release 状态失败')
      })
    }, 1_000)
    return () => window.clearInterval(timer)
  }, [hasActiveRelease, loadReleases])

  return (
    <section className="project-subpage" aria-labelledby="releases-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">RNACOS PUBLICATION</p>
          <h2 id="releases-title">Release 记录</h2>
          <p>每个 Release 独立记录 rnacos 写入和回读；实例激活证据始终单独展示。</p>
        </div>
        <button
          className="button-secondary"
          disabled={loading || refreshing}
          onClick={() =>
            void loadReleases(undefined, true).catch((error: unknown) => {
              setErrorMessage(error instanceof Error ? error.message : '刷新 Release 状态失败')
            })
          }
          type="button"
        >
          {refreshing ? '刷新中…' : '刷新状态'}
        </button>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>状态加载失败</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      {hasActiveRelease ? (
        <div className="release-polling-state" role="status">
          <span className="connection-dot connection-dot-loading" aria-hidden="true" />
          发布正在进行，页面会自动刷新
        </div>
      ) : null}

      {loading ? (
        <div className="route-empty-state">正在加载 Release 记录…</div>
      ) : releases.length === 0 ? (
        <div className="route-empty-state">
          <h3>尚未创建 Release</h3>
          <p>请从 Versions 页选择一个不可变配置版本创建 Release。</p>
        </div>
      ) : (
        <ol className="release-timeline">
          {releases.map((release) => (
            <li className="release-card" key={release.id}>
              <header>
                <div>
                  <span className="release-sequence">R{release.sequence}</span>
                  <div>
                    <strong>{release.title}</strong>
                    <small>
                      来源 V{release.sourceConfigurationVersion.number} · wire v
                      {release.allocatedWireVersion} ·{' '}
                      {new Date(release.createdAt).toLocaleString('zh-CN')}
                    </small>
                  </div>
                </div>
                <div className="release-state-stack">
                  <span className="status-chip status-chip-unknown">{release.status}</span>
                  <small>实例激活：未知</small>
                </div>
              </header>
              {release.description ? <p>{release.description}</p> : null}
              <div className="release-resources">
                {release.resources.map((resource) => (
                  <div key={resource.id}>
                    <span>
                      <strong>
                        {resource.kind === 'project_route' ? 'Project route' : 'Project list'}
                      </strong>
                      <small>
                        {resource.dataId} · {resource.group}
                      </small>
                    </span>
                    <span className="status-chip status-chip-unknown">{resource.status}</span>
                  </div>
                ))}
              </div>
            </li>
          ))}
        </ol>
      )}
    </section>
  )
}
