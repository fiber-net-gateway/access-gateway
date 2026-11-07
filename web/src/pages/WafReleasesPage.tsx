import { useCallback, useEffect, useMemo, useState } from 'react'

import {
  compileWafPolicyPreview,
  createWafRelease,
  fetchWafEdgeFunctions,
  fetchWafPolicyDraft,
  fetchWafReleases,
  queueWafReleasePublication,
  rollbackWafRelease,
} from '../api/client'
import type { WafReleaseView } from '../api/types'
import {
  ActivationEvidencePanel,
  activationChip,
  activationLabel,
} from '../components/ActivationEvidencePanel'

const terminalReleaseStatuses = new Set([
  'published',
  'partially_published',
  'publish_failed',
  'validation_failed',
  'canceled',
  'superseded',
  'abandoned',
])

// Releases that still carry publishable payloads and can be rolled back to.
const rollbackEligibleStatuses = new Set([
  'ready',
  'published',
  'partially_published',
  'publish_failed',
  'superseded',
])

interface DiffLine {
  kind: 'context' | 'added' | 'removed'
  text: string
}

// Prefix/suffix line diff: the generator output is deterministic and mostly
// line-aligned, so trimming the common head/tail localizes the change without
// an O(n·m) LCS over scripts that can reach thousands of lines.
function diffScript(previous: string, next: string): DiffLine[] {
  const a = previous.split('\n')
  const b = next.split('\n')
  let start = 0
  while (start < a.length && start < b.length && a[start] === b[start]) start += 1
  let endA = a.length
  let endB = b.length
  while (endA > start && endB > start && a[endA - 1] === b[endB - 1]) {
    endA -= 1
    endB -= 1
  }
  const contextStart = Math.max(0, start - 2)
  const contextEndA = Math.min(a.length, endA + 2)
  const lines: DiffLine[] = []
  for (let index = contextStart; index < start; index += 1) {
    lines.push({ kind: 'context', text: a[index] ?? '' })
  }
  for (let index = start; index < endA; index += 1) {
    lines.push({ kind: 'removed', text: a[index] ?? '' })
  }
  for (let index = start; index < endB; index += 1) {
    lines.push({ kind: 'added', text: b[index] ?? '' })
  }
  for (let index = endA; index < contextEndA; index += 1) {
    lines.push({ kind: 'context', text: a[index] ?? '' })
  }
  return lines
}

export function WafReleasesPage() {
  const [releases, setReleases] = useState<readonly WafReleaseView[]>([])
  const [loading, setLoading] = useState(true)
  const [refreshing, setRefreshing] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [actionError, setActionError] = useState<string | null>(null)
  const [working, setWorking] = useState<string | null>(null)

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
        setReleases(await fetchWafReleases(signal))
        setErrorMessage(null)
      } finally {
        setLoading(false)
        setRefreshing(false)
      }
    },
    [],
  )

  useEffect(() => {
    const controller = new AbortController()
    void loadReleases(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 WAF Release 失败')
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
          setErrorMessage(error instanceof Error ? error.message : '刷新 WAF Release 状态失败')
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

  const runAction = async (key: string, action: () => Promise<unknown>): Promise<void> => {
    setWorking(key)
    setActionError(null)
    try {
      await action()
      await loadReleases(undefined, true)
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '操作失败')
    } finally {
      setWorking(null)
    }
  }

  const createFromDraft = async (): Promise<void> => {
    if (!window.confirm('用当前已保存的 WAF 策略草稿创建新的边缘函数 Release 吗？')) return
    await runAction('create', createWafRelease)
  }

  const publishRelease = async (release: WafReleaseView): Promise<void> => {
    if (
      !window.confirm(
        `发布 R${release.sequence} 到 rnacos 吗？脚本先写、索引后写，失败时保留旧快照。`,
      )
    ) {
      return
    }
    await runAction(`publish-${release.id}`, () => queueWafReleasePublication(release.id))
  }

  const rollbackToRelease = async (release: WafReleaseView): Promise<void> => {
    if (
      !window.confirm(
        `回滚到 R${release.sequence} 吗？将基于该 Release 的历史载荷创建并发布一个新的 Release，历史不会被改写。`,
      )
    ) {
      return
    }
    await runAction(`rollback-${release.id}`, () => rollbackWafRelease(release.id))
  }

  return (
    <section className="project-subpage" aria-labelledby="waf-releases-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">WAF PUBLICATION</p>
          <h2 id="waf-releases-title">WAF 发布</h2>
          <p>编译结果先预览确认，再创建 Release；回滚会从历史载荷生成新的 Release。</p>
        </div>
        <div className="header-actions">
          <button
            className="button-secondary"
            disabled={loading || refreshing}
            onClick={() =>
              void loadReleases(undefined, true).catch((error: unknown) => {
                setErrorMessage(
                  error instanceof Error ? error.message : '刷新 WAF Release 状态失败',
                )
              })
            }
            type="button"
          >
            {refreshing ? '刷新中…' : '刷新状态'}
          </button>
          <button className="button-primary" onClick={() => void createFromDraft()} type="button">
            从草稿创建 Release
          </button>
        </div>
      </header>

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

      <WafPublicationPreview
        onCreateFromDraft={() => void createFromDraft()}
        working={working === 'create'}
      />

      <h3>Release 记录</h3>
      {loading ? (
        <div className="route-empty-state">正在加载 WAF Release…</div>
      ) : releases.length === 0 ? (
        <div className="route-empty-state">
          <h3>尚未创建 WAF Release</h3>
          <p>保存策略草稿后，点击“从草稿创建 Release”开始第一次发布。</p>
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
                      {release.mode === 'mounted'
                        ? `挂载函数 ${release.functionName} · ${release.ruleCount} 条规则`
                        : '卸载边缘函数（空索引）'}
                    </strong>
                    <small>
                      索引版本 {release.indexVersion} ·{' '}
                      {release.scriptSha256
                        ? `脚本 ${release.scriptSha256.slice(0, 12)}…`
                        : '无脚本'}{' '}
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
                        {resource.kind === 'edge_function_script' ? '函数脚本' : '函数索引'}
                      </strong>
                      <small>
                        {resource.dataId} · {resource.group}
                        {resource.verifiedSha256
                          ? ` · 校验 ${resource.verifiedSha256.slice(0, 12)}…`
                          : ''}
                      </small>
                    </span>
                    <span className="status-chip status-chip-unknown">{resource.status}</span>
                  </div>
                ))}
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

interface WafPublicationPreviewProps {
  onCreateFromDraft(): void
  working: boolean
}

// 发布预览 (§4.4): compile the SAVED draft, then diff the generated script
// against the live script currently in rnacos — "what would change on the
// wire", independent of release history.
function WafPublicationPreview({ onCreateFromDraft, working }: WafPublicationPreviewProps) {
  const [previewError, setPreviewError] = useState<string | null>(null)
  const [preview, setPreview] = useState<{
    script: string | null
    valid: boolean
    issueCount: number
  } | null>(null)
  const [live, setLive] = useState<string | null>(null)

  useEffect(() => {
    const controller = new AbortController()
    void (async () => {
      try {
        const [draft, edgeFunctions] = await Promise.all([
          fetchWafPolicyDraft(controller.signal),
          fetchWafEdgeFunctions(controller.signal).catch(() => null),
        ])
        setLive(edgeFunctions?.scripts.find((script) => script.name === 'waf')?.content ?? null)
        // Compile the exact saved policy — the same bytes a Release would use.
        const result = await compileWafPolicyPreview(draft.policy, controller.signal)
        setPreview({
          script: result.compilation.kind === 'mounted' ? result.compilation.script : null,
          valid: result.valid,
          issueCount: result.issues.length,
        })
        setPreviewError(null)
      } catch (error) {
        if (!controller.signal.aborted) {
          setPreviewError(error instanceof Error ? error.message : '加载发布预览失败')
        }
      }
    })()
    return () => controller.abort()
  }, [])

  const diff = preview?.script != null && live !== null ? diffScript(live, preview.script) : null

  return (
    <section className="settings-card" aria-labelledby="waf-publish-preview-title">
      <h3 id="waf-publish-preview-title">发布预览</h3>
      {previewError ? (
        <div className="error-banner" role="alert">
          <strong>发布预览失败</strong>
          <span>{previewError}</span>
        </div>
      ) : preview === null ? (
        <p role="status">正在编译已保存的草稿…</p>
      ) : (
        <div className="waf-compile-summary">
          <p>
            <span className={`status-chip status-chip-${preview.valid ? 'ready' : 'pending'}`}>
              {preview.valid ? '草稿可编译' : `${preview.issueCount} 个问题`}
            </span>{' '}
            {preview.script
              ? `生成脚本 ${preview.script.split('\n').length} 行`
              : '当前策略不生成脚本（发布空索引卸载）'}
          </p>
          {preview.script && live === null ? (
            <p className="settings-note">
              rnacos 中当前没有已挂载的 WAF 脚本，首次发布将全量写入。
            </p>
          ) : null}
          {preview.script && diff ? (
            diff.every((line) => line.kind === 'context') ? (
              <p className="settings-note">生成脚本与 rnacos 当前脚本一致，发布不会改变内容。</p>
            ) : (
              <details className="wire-preview">
                <summary>
                  与 rnacos 当前脚本的差异（+
                  {diff.filter((line) => line.kind === 'added').length} / -
                  {diff.filter((line) => line.kind === 'removed').length} 行）
                </summary>
                <pre className="waf-script-diff">
                  {diff.map((line, index) => (
                    <span
                      className={`waf-diff-line waf-diff-${line.kind}`}
                      key={`${index}-${line.text}`}
                    >
                      {line.kind === 'added' ? '+' : line.kind === 'removed' ? '-' : ' '}{' '}
                      {line.text}
                      {'\n'}
                    </span>
                  ))}
                </pre>
              </details>
            )
          ) : null}
          <div className="form-actions">
            <button
              className="button-primary"
              disabled={working || !preview.valid}
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
