import { useCallback, useEffect, useState } from 'react'

import {
  ApiClientError,
  createGrayRelease,
  createGrayRulesDraft,
  fetchGrayLive,
  fetchGrayReleases,
  fetchGrayRulesDraft,
  queueGrayReleasePublication,
  rollbackGrayRelease,
  validateGrayRulesPreview,
} from '../api/client'
import type { GrayLiveView, GrayReleaseView, GrayValidationPreviewView } from '../api/types'
import {
  ActivationEvidencePanel,
  activationChip,
  activationLabel,
} from '../components/ActivationEvidencePanel'

const inFlightStatuses = new Set(['queued', 'publishing'])
const rollbackEligibleStatuses = new Set(['published', 'superseded', 'partially_published'])

function nextRefreshDelay(releases: readonly GrayReleaseView[]): number | null {
  if (releases.some((release) => inFlightStatuses.has(release.status))) return 2_000
  if (
    releases.some(
      (release) => release.status === 'published' && release.activationStatus === 'pending',
    )
  ) {
    return 5_000
  }
  return null
}

const releaseStatusLabels: Record<string, string> = {
  ready: '待发布',
  queued: '排队中',
  publishing: '发布中',
  published: '已发布',
  partially_published: '部分发布',
  publish_failed: '发布失败',
  superseded: '已取代',
  validation_failed: '校验失败',
}

export function GrayReleasesPage() {
  const [releases, setReleases] = useState<readonly GrayReleaseView[]>([])
  const [loading, setLoading] = useState(true)
  const [refreshing, setRefreshing] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [actionError, setActionError] = useState<string | null>(null)
  const [working, setWorking] = useState<string | null>(null)
  const [description, setDescription] = useState('')
  const [previewKey, setPreviewKey] = useState(0)

  const loadReleases = useCallback(async (signal?: AbortSignal, silent = false) => {
    if (silent) setRefreshing(true)
    else setLoading(true)
    try {
      const items = await fetchGrayReleases(signal)
      if (!signal?.aborted) {
        setReleases(items)
        setErrorMessage(null)
      }
    } catch (error) {
      if (!signal?.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载灰度 Release 失败')
      }
    } finally {
      if (!signal?.aborted) {
        setLoading(false)
        setRefreshing(false)
      }
    }
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    void loadReleases(controller.signal)
    return () => controller.abort()
  }, [loadReleases])

  const refreshDelay = nextRefreshDelay(releases)
  useEffect(() => {
    if (refreshDelay === null) return
    const controller = new AbortController()
    const timer = window.setTimeout(() => {
      void loadReleases(controller.signal, true)
    }, refreshDelay)
    return () => {
      window.clearTimeout(timer)
      controller.abort()
    }
  }, [refreshDelay, loadReleases])

  const runAction = async (key: string, action: () => Promise<unknown>): Promise<void> => {
    setWorking(key)
    setActionError(null)
    try {
      await action()
      await loadReleases(undefined, true)
      setPreviewKey((value) => value + 1)
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '操作失败')
    } finally {
      setWorking(null)
    }
  }

  const createFromDraft = async (): Promise<void> => {
    if (!window.confirm('用当前已保存的灰度规则草稿创建新的不可变 Release 吗？')) return
    await runAction('create', async () => {
      await createGrayRelease(description.trim() || undefined)
      setDescription('')
    })
  }

  const publishRelease = async (release: GrayReleaseView): Promise<void> => {
    if (
      !window.confirm(
        `发布 R${release.sequence} 到 rnacos（ploto.unified-access.gray-match）吗？数据面将在监听回调中校验，非法候选会保留旧快照。`,
      )
    ) {
      return
    }
    await runAction(`publish-${release.id}`, () => queueGrayReleasePublication(release.id))
  }

  const rollbackToRelease = async (release: GrayReleaseView): Promise<void> => {
    if (
      !window.confirm(
        `回滚到 R${release.sequence} 吗？将基于该 Release 的历史载荷创建一条新的不可变 Release，历史记录不会被改写。`,
      )
    ) {
      return
    }
    await runAction(`rollback-${release.id}`, () => rollbackGrayRelease(release.id))
  }

  return (
    <section className="project-subpage" aria-label="灰度发布">
      <div className="subpage-meta subpage-meta-actions">
        <p className="subpage-lead">
          每次发布都先生成不可变 Release，再由发布 Worker 写入 rnacos 并校验读回；回滚会基于历史
          Release 载荷创建新 Release。
        </p>
        <div className="header-actions">
          <button
            className="button-secondary"
            disabled={loading || refreshing}
            onClick={() => void loadReleases(undefined, true)}
            type="button"
          >
            {refreshing ? '刷新中…' : '刷新状态'}
          </button>
        </div>
      </div>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>状态加载失败</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}
      {actionError ? (
        <div className="error-banner" role="alert">
          <strong>发布操作未完成</strong>
          <span>{actionError}</span>
        </div>
      ) : null}

      {refreshDelay !== null ? (
        <div className="release-polling-state" role="status">
          <span className="connection-dot connection-dot-loading" aria-hidden="true" />
          发布或实例激活正在推进，页面会自动刷新
        </div>
      ) : null}

      <GrayPublicationPreview
        description={description}
        onCreateFromDraft={() => void createFromDraft()}
        onDescriptionChange={setDescription}
        reloadKey={previewKey}
        working={working === 'create'}
      />

      <h3>Release 记录</h3>
      {loading ? (
        <div className="route-empty-state">正在加载灰度 Release…</div>
      ) : releases.length === 0 ? (
        <div className="route-empty-state">
          <h3>尚未创建灰度 Release</h3>
          <p>保存灰度规则草稿后，点击“从草稿创建 Release”开始第一次发布。</p>
        </div>
      ) : (
        <ol className="release-timeline">
          {releases.map((release) => (
            <li className="release-card" key={release.id}>
              <header>
                <div>
                  <span className="release-sequence">R{release.sequence}</span>
                  <div>
                    <strong>
                      {release.ruleCount > 0
                        ? `${release.ruleCount} 条有效灰度规则（${release.entries.join(', ') || '无入口'}）`
                        : '清空灰度规则（{}）'}
                    </strong>
                    <small>
                      Wire SHA-256：{release.wireSha256.slice(0, 12)}… ·{' '}
                      {new Date(release.createdAt).toLocaleString('zh-CN')}
                    </small>
                  </div>
                </div>
                <div className="release-state-stack">
                  <span className="status-chip status-chip-neutral">
                    发布：{releaseStatusLabels[release.status] ?? release.status}
                  </span>
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
                <div>
                  <span>
                    <strong>灰度规则</strong>
                    <small>
                      {release.resource.dataId} · {release.resource.group}
                      {release.resource.verifiedSha256
                        ? ` · 读回校验 ${release.resource.verifiedSha256.slice(0, 12)}…`
                        : ''}
                    </small>
                  </span>
                  <span className="status-chip status-chip-unknown">{release.resource.status}</span>
                </div>
              </div>
              <div className="header-actions">
                {release.status === 'ready' ? (
                  <button
                    className="button-primary"
                    disabled={working !== null}
                    onClick={() => void publishRelease(release)}
                    type="button"
                  >
                    {working === `publish-${release.id}` ? '发布中…' : '发布到 rnacos'}
                  </button>
                ) : null}
                {rollbackEligibleStatuses.has(release.status) ? (
                  <button
                    className="button-secondary"
                    disabled={working !== null}
                    onClick={() => void rollbackToRelease(release)}
                    type="button"
                  >
                    {working === `rollback-${release.id}` ? '回滚中…' : '回滚到此版本'}
                  </button>
                ) : null}
                {release.publication.state ? (
                  <small>发布任务：{release.publication.state}</small>
                ) : null}
              </div>
              <ActivationEvidencePanel releaseId={release.id} />
            </li>
          ))}
        </ol>
      )}
    </section>
  )
}

interface GrayPublicationPreviewProps {
  description: string
  onDescriptionChange(value: string): void
  onCreateFromDraft(): void
  working: boolean
  reloadKey: number
}

function GrayPublicationPreview({
  description,
  onDescriptionChange,
  onCreateFromDraft,
  working,
  reloadKey,
}: GrayPublicationPreviewProps) {
  const [previewError, setPreviewError] = useState<string | null>(null)
  const [state, setState] = useState<{
    validation: GrayValidationPreviewView
    live: GrayLiveView | null
  } | null>(null)

  useEffect(() => {
    const controller = new AbortController()
    void (async () => {
      try {
        const [draft, live] = await Promise.all([
          fetchGrayRulesDraft(controller.signal).catch(async (error: unknown) => {
            if (error instanceof ApiClientError && error.status === 404) {
              return createGrayRulesDraft()
            }
            throw error
          }),
          fetchGrayLive(controller.signal).catch(() => null),
        ])
        const validation = await validateGrayRulesPreview(draft.document, controller.signal)
        if (!controller.signal.aborted) {
          setState({ validation, live })
          setPreviewError(null)
        }
      } catch (error) {
        if (!controller.signal.aborted) {
          setPreviewError(error instanceof Error ? error.message : '加载发布预览失败')
        }
      }
    })()
    return () => controller.abort()
  }, [reloadKey])

  const isSameAsLive =
    state !== null &&
    state.live?.exists === true &&
    state.live.sha256 !== null &&
    state.validation.wireSha256 === state.live.sha256

  return (
    <section className="settings-card" aria-labelledby="gray-publish-preview-title">
      <h3 id="gray-publish-preview-title">发布预览</h3>
      {previewError ? (
        <div className="error-banner" role="alert">
          <strong>发布预览失败</strong>
          <span>{previewError}</span>
        </div>
      ) : state === null ? (
        <p role="status">正在校验已保存的灰度草稿…</p>
      ) : (
        <div className="waf-compile-summary">
          <p>
            <span
              className={`status-chip status-chip-${
                state.validation.valid && state.validation.nativeValid !== false
                  ? 'ready'
                  : 'pending'
              }`}
            >
              {state.validation.valid && state.validation.nativeValid !== false
                ? '草稿可发布'
                : '草稿存在校验问题'}
            </span>{' '}
            <span
              className={`status-chip status-chip-${
                state.validation.nativeValid === true
                  ? 'ready'
                  : state.validation.nativeValid === false
                    ? 'pending'
                    : 'unknown'
              }`}
            >
              Native Validator：
              {state.validation.nativeValid === true
                ? '已通过'
                : state.validation.nativeValid === false
                  ? '拒绝'
                  : '不可用'}
            </span>{' '}
            {state.live === null
              ? '未能读取 rnacos 当前值'
              : !state.live.exists
                ? 'rnacos 尚无该配置，本次将首次写入'
                : isSameAsLive
                  ? '草稿编译结果与 rnacos 当前内容一致'
                  : '草稿编译结果与 rnacos 当前内容存在差异'}
          </p>

          {state.validation.wirePreview !== null ? (
            <details className="wire-preview">
              <summary>
                查看待发布 Wire JSON（{state.validation.dataId} · {state.validation.group}）
              </summary>
              <pre>{state.validation.wirePreview}</pre>
            </details>
          ) : null}

          {state.live?.exists && state.live.content !== null ? (
            <details className="wire-preview">
              <summary>
                查看 rnacos 当前内容（SHA-256：{state.live.sha256?.slice(0, 12) ?? '—'}…）
              </summary>
              <pre>{state.live.content}</pre>
            </details>
          ) : null}

          <label className="gray-field">
            <span>发布说明（可选）</span>
            <input
              aria-label="灰度发布说明"
              maxLength={255}
              placeholder="例如：公网入口灰度放量至 5%"
              type="text"
              value={description}
              onChange={(event) => onDescriptionChange(event.target.value)}
            />
          </label>

          <div className="form-actions">
            <button
              className="button-primary"
              disabled={
                working || !state.validation.valid || state.validation.nativeValid === false
              }
              onClick={onCreateFromDraft}
              type="button"
            >
              {working ? '创建中…' : '从草稿创建 Release'}
            </button>
          </div>
        </div>
      )}
    </section>
  )
}
