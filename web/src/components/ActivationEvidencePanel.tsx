import { useCallback, useEffect, useRef, useState } from 'react'

import { fetchReleaseActivation } from '../api/client'
import type { ActivationInstanceList, ActivationStatus } from '../api/types'

const activationLabels: Record<ActivationStatus, string> = {
  unknown: '未知',
  pending: '等待激活',
  active: '已激活',
  degraded: '异常',
}

export function activationLabel(status: ActivationStatus): string {
  return activationLabels[status]
}

export function activationChip(status: ActivationStatus): string {
  return status === 'active' ? 'ready' : status
}

export function ActivationEvidencePanel({ releaseId }: { releaseId: string }) {
  const [expanded, setExpanded] = useState(false)
  const [detail, setDetail] = useState<ActivationInstanceList | null>(null)
  const [loading, setLoading] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const requestedItems = useRef(50)

  const loadSnapshot = useCallback(
    async (signal: AbortSignal): Promise<void> => {
      let page = await fetchReleaseActivation(releaseId, null, signal)
      const items = [...page.items]
      while (page.nextCursor && items.length < requestedItems.current) {
        page = await fetchReleaseActivation(releaseId, page.nextCursor, signal)
        items.push(...page.items)
      }
      setDetail({ ...page, items })
      setErrorMessage(null)
    },
    [releaseId],
  )

  useEffect(() => {
    requestedItems.current = 50
    setDetail(null)
    setExpanded(false)
    setErrorMessage(null)
  }, [releaseId])

  useEffect(() => {
    if (!expanded) return
    const controller = new AbortController()
    let timer: number | null = null
    const refresh = async (initial: boolean): Promise<void> => {
      if (initial) setLoading(true)
      try {
        await loadSnapshot(controller.signal)
      } catch (error) {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : '加载实例激活证据失败')
        }
      } finally {
        if (initial) setLoading(false)
        if (!controller.signal.aborted) {
          timer = window.setTimeout(() => void refresh(false), 5_000)
        }
      }
    }
    void refresh(true)
    return () => {
      controller.abort()
      if (timer !== null) window.clearTimeout(timer)
    }
  }, [expanded, loadSnapshot])

  const loadMore = async (): Promise<void> => {
    if (!detail?.nextCursor) return
    setLoading(true)
    const controller = new AbortController()
    try {
      const nextPage = await fetchReleaseActivation(releaseId, detail.nextCursor, controller.signal)
      const items = [...detail.items, ...nextPage.items]
      requestedItems.current = items.length
      setDetail({ ...nextPage, items })
      setErrorMessage(null)
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '加载更多实例证据失败')
    } finally {
      setLoading(false)
    }
  }

  const panelId = `activation-evidence-${releaseId}`
  return (
    <div className="activation-evidence-control">
      <button
        aria-controls={panelId}
        aria-expanded={expanded}
        className="button-link"
        onClick={() => setExpanded((current) => !current)}
        type="button"
      >
        {expanded ? '收起实例证据' : '查看实例证据'}
      </button>
      {expanded ? (
        <div className="activation-evidence-panel" id={panelId}>
          {errorMessage ? (
            <p className="activation-evidence-error" role="alert">
              {errorMessage}
            </p>
          ) : null}
          {loading && !detail ? <p role="status">正在加载实例证据…</p> : null}
          {detail?.items.length === 0 ? (
            <p>没有配置必需的 access-server 实例，因此激活状态保持未知。</p>
          ) : null}
          {detail && detail.items.length > 0 ? (
            <ul>
              {detail.items.map((instance) => (
                <li key={instance.id}>
                  <span>
                    <strong>{instance.instanceKey}</strong>
                    <small>
                      build {instance.buildVersion ?? '未知'} · snapshot{' '}
                      {instance.routeSnapshotGeneration ?? '未知'}
                    </small>
                  </span>
                  <span>
                    <span className={`status-chip status-chip-${activationChip(instance.status)}`}>
                      {activationLabel(instance.status)}
                    </span>
                    <small>
                      {instance.candidateErrorCode
                        ? `错误：${instance.candidateErrorCode}`
                        : `候选：${instance.candidateStatus ?? '未观察到'}`}
                    </small>
                    <small>
                      {instance.observedAt
                        ? new Date(instance.observedAt).toLocaleString('zh-CN')
                        : '尚无采集证据'}
                    </small>
                  </span>
                </li>
              ))}
            </ul>
          ) : null}
          {detail?.nextCursor ? (
            <button
              className="button-secondary"
              disabled={loading}
              onClick={() => void loadMore()}
              type="button"
            >
              {loading ? '加载中…' : '加载更多实例'}
            </button>
          ) : null}
        </div>
      ) : null}
    </div>
  )
}
