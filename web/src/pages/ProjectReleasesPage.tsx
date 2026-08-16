import { useCallback, useEffect, useMemo, useState } from 'react'

import { fetchProjectReleases } from '../api/client'
import type { ProjectReleaseView } from '../api/types'
import {
  ActivationEvidencePanel,
  activationChip,
  activationLabel,
} from '../components/ActivationEvidencePanel'
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
  const refreshDelay = useMemo(() => {
    if (releases.some((release) => !terminalReleaseStatuses.has(release.status))) return 1_000
    if (
      releases.some(
        (release) => release.status === 'published' && release.activation.targetCount > 0,
      )
    ) {
      return 5_000
    }
    return null
  }, [releases])

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
    if (refreshDelay === null) return
    const delay = refreshDelay
    const controller = new AbortController()
    let timer = window.setTimeout(refresh, delay)
    async function refresh(): Promise<void> {
      try {
        await loadReleases(controller.signal, true)
      } catch (error) {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : '刷新 Release 状态失败')
        }
      } finally {
        if (!controller.signal.aborted) timer = window.setTimeout(refresh, delay)
      }
    }
    return () => {
      controller.abort()
      window.clearTimeout(timer)
    }
  }, [loadReleases, refreshDelay])

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

      {refreshDelay !== null ? (
        <div className="release-polling-state" role="status">
          <span className="connection-dot connection-dot-loading" aria-hidden="true" />
          发布或实例激活正在推进，页面会自动刷新
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
                      {release.kind === 'project_decommission'
                        ? 'Project 下线'
                        : `来源 V${release.sourceConfigurationVersion?.number ?? '未知'} · wire v${
                            release.allocatedWireVersion ?? '未知'
                          }`}{' '}
                      · {new Date(release.createdAt).toLocaleString('zh-CN')}
                    </small>
                  </div>
                </div>
                <div className="release-state-stack">
                  <span className="status-chip status-chip-unknown">{release.status}</span>
                  <span
                    className={`status-chip status-chip-${activationChip(release.activationStatus)}`}
                  >
                    实例：{activationLabel(release.activationStatus)}
                  </span>
                  <small>
                    {release.activation.activeCount}/{release.activation.targetCount} 已激活
                  </small>
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
                        {resource.dataId} · {resource.group} ·{' '}
                        {resource.operation === 'upsert' ? '写入' : '删除'}
                      </small>
                    </span>
                    <span className="status-chip status-chip-unknown">{resource.status}</span>
                  </div>
                ))}
              </div>
              <ActivationEvidencePanel releaseId={release.id} />
            </li>
          ))}
        </ol>
      )}
    </section>
  )
}
