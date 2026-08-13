import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router'

import {
  createProjectDecommissionRelease,
  fetchProjectReleases,
  queueReleasePublication,
} from '../api/client'
import type { ProjectReleaseView } from '../api/types'
import { useProjectContext } from './ProjectLayout'

const activeReleaseStatuses = new Set(['creating', 'validating', 'ready', 'queued', 'publishing'])

function formatTime(value: string): string {
  return new Date(value).toLocaleString('zh-CN')
}

function resourceOperation(operation: 'upsert' | 'remove'): string {
  return operation === 'upsert' ? '写入目标列表' : '删除资源'
}

export function ProjectSettingsPage() {
  const { project, systemStatus } = useProjectContext()
  const navigate = useNavigate()
  const openButton = useRef<HTMLButtonElement>(null)
  const [releases, setReleases] = useState<readonly ProjectReleaseView[]>([])
  const [loading, setLoading] = useState(true)
  const [submitting, setSubmitting] = useState(false)
  const [dialogOpen, setDialogOpen] = useState(false)
  const [confirmationDomain, setConfirmationDomain] = useState('')
  const [reason, setReason] = useState('')
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const publicationCapability = systemStatus?.dependencies.publicationWorker
  const publicationReady = publicationCapability?.status === 'ready'

  const loadReleases = useCallback(
    async (signal?: AbortSignal): Promise<void> => {
      setLoading(true)
      try {
        setReleases(await fetchProjectReleases(project.id, signal))
        setErrorMessage(null)
      } finally {
        setLoading(false)
      }
    },
    [project.id],
  )

  useEffect(() => {
    const controller = new AbortController()
    void loadReleases(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 Project 生命周期失败')
      }
    })
    return () => controller.abort()
  }, [loadReleases])

  const latestDecommission =
    releases.find((release) => release.kind === 'project_decommission') ?? null
  const decommissionInProgress =
    latestDecommission !== null && activeReleaseStatuses.has(latestDecommission.status)
  const canCreate =
    project.status !== 'archived' &&
    !decommissionInProgress &&
    latestDecommission?.status !== 'published'
  const confirmationValid = confirmationDomain.trim() === project.domain && reason.trim().length > 0

  const closeDialog = (): void => {
    setDialogOpen(false)
    window.requestAnimationFrame(() => openButton.current?.focus())
  }

  const submitDecommission = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    if (!confirmationValid) return
    setSubmitting(true)
    setErrorMessage(null)
    try {
      const prepared = await createProjectDecommissionRelease(
        project.id,
        project.lockVersion,
        confirmationDomain.trim(),
        reason,
      )
      setReleases((current) => [prepared, ...current.filter((item) => item.id !== prepared.id)])
      setDialogOpen(false)
      await queueReleasePublication(prepared.id)
      await navigate('../releases')
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '创建下线 Release 失败')
    } finally {
      setSubmitting(false)
    }
  }

  const continuePublication = async (): Promise<void> => {
    if (!latestDecommission || latestDecommission.status !== 'ready') return
    setSubmitting(true)
    setErrorMessage(null)
    try {
      await queueReleasePublication(latestDecommission.id)
      await navigate('../releases')
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '下线 Release 排队失败')
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <section className="project-subpage project-settings-page" aria-labelledby="settings-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT LIFECYCLE</p>
          <h2 id="settings-title">Settings</h2>
          <p>查看不可变身份，并通过有发布证据的 Release 下线域名。</p>
        </div>
        <span
          className={`status-chip status-chip-${project.status === 'active' ? 'ready' : 'pending'}`}
        >
          {project.status === 'active' ? 'Active' : '下线处理中'}
        </span>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      <div className="settings-summary-grid">
        <article className="settings-card">
          <p className="eyebrow">PROJECT IDENTITY</p>
          <h3>不可变身份</h3>
          <dl className="settings-facts">
            <div>
              <dt>域名</dt>
              <dd>{project.domain}</dd>
            </div>
            <div>
              <dt>Project ID</dt>
              <dd className="settings-monospace">{project.id}</dd>
            </div>
            <div>
              <dt>创建时间</dt>
              <dd>{formatTime(project.createdAt)}</dd>
            </div>
            <div>
              <dt>最后更新</dt>
              <dd>{formatTime(project.updatedAt)}</dd>
            </div>
          </dl>
          <p className="settings-note">
            域名同时是 exact Host 与 rnacos project key，不能原地改名。请创建新域名后再下线旧域名。
          </p>
        </article>

        <article className="settings-card">
          <p className="eyebrow">LIFECYCLE EVIDENCE</p>
          <h3>配置与运行证据</h3>
          <dl className="settings-facts">
            <div>
              <dt>当前配置</dt>
              <dd>{project.draft?.revision ? `V${project.draft.revision}` : '尚无版本'}</dd>
            </div>
            <div>
              <dt>rnacos 已发布来源</dt>
              <dd>{project.publishedVersion ? `V${project.publishedVersion}` : '尚未发布'}</dd>
            </div>
            <div>
              <dt>实例激活</dt>
              <dd>未知</dd>
            </div>
            <div>
              <dt>Project ETag</dt>
              <dd>{project.lockVersion}</dd>
            </div>
          </dl>
          <p className="settings-note">
            rnacos 写入和回读不能证明具体 access-server 实例已经卸载配置。
          </p>
        </article>
      </div>

      {loading ? (
        <div className="route-empty-state">正在加载下线 Release…</div>
      ) : latestDecommission ? (
        <article className="settings-release-card" aria-label="最近下线 Release">
          <div>
            <p className="eyebrow">LATEST DECOMMISSION RELEASE</p>
            <h3>
              R{latestDecommission.sequence} · {latestDecommission.title}
            </h3>
            <p>{latestDecommission.description}</p>
          </div>
          <div className="settings-release-evidence">
            <span className="status-chip status-chip-unknown">{latestDecommission.status}</span>
            <span>实例激活：未知</span>
            {latestDecommission.resources.map((resource) => (
              <span key={resource.id}>
                Project List · {resourceOperation(resource.operation)} · {resource.status}
              </span>
            ))}
          </div>
          {latestDecommission.status === 'ready' ? (
            <button
              className="button-primary"
              disabled={!publicationReady || submitting}
              onClick={() => void continuePublication()}
              type="button"
            >
              {submitting ? '正在排队…' : '继续发布下线 Release'}
            </button>
          ) : null}
        </article>
      ) : null}

      <section className="danger-zone" aria-labelledby="danger-zone-title">
        <div>
          <p className="eyebrow">DANGER ZONE</p>
          <h3 id="danger-zone-title">下线并归档 Project</h3>
          <p>
            系统会先创建不可变 Release，再从 rnacos Project List 移除域名并回读。历史版本、Release
            和审计记录不会被删除。
          </p>
          <ul>
            <li>不会发布浏览器中未保存的 Route 修改。</li>
            <li>不会用空 Route 或空 YAML 表达下线。</li>
            <li>发布成功后仍只报告“实例激活未知”。</li>
          </ul>
          {!publicationReady ? (
            <div className="capability-notice" role="note">
              rnacos 发布不可用：{publicationCapability?.detail ?? '系统能力尚未加载'}
            </div>
          ) : null}
        </div>
        <button
          className="button-danger"
          disabled={!publicationReady || !canCreate || loading || submitting}
          onClick={() => {
            setConfirmationDomain('')
            setReason('')
            setDialogOpen(true)
          }}
          ref={openButton}
          type="button"
        >
          {project.status === 'decommissioning' ? '重新创建下线 Release' : '下线并归档 Project'}
        </button>
      </section>

      {dialogOpen ? (
        <div className="dialog-backdrop" role="presentation">
          <form
            aria-labelledby="decommission-title"
            className="dialog-card dialog-card-danger"
            onKeyDown={(event) => {
              if (event.key === 'Escape' && !submitting) closeDialog()
            }}
            onSubmit={(event) => void submitDecommission(event)}
            aria-modal="true"
            role="dialog"
          >
            <p className="eyebrow">CREATE & PUBLISH DECOMMISSION RELEASE</p>
            <h2 id="decommission-title">确认下线 {project.domain}</h2>
            <div className="dialog-warning">
              Project List 回读成功只证明 rnacos 已发布；在实例证据接入前，不代表所有实例已经下线。
            </div>
            <label>
              下线原因
              <textarea
                autoFocus
                maxLength={1000}
                required
                rows={4}
                value={reason}
                onChange={(event) => setReason(event.target.value)}
              />
            </label>
            <label>
              输入完整域名以确认
              <input
                autoComplete="off"
                placeholder={project.domain}
                required
                value={confirmationDomain}
                onChange={(event) => setConfirmationDomain(event.target.value)}
              />
            </label>
            <p className="dialog-footnote">
              预期 Project ETag：{project.lockVersion}。若 Project 已变化，服务端会拒绝本次操作。
            </p>
            <div className="dialog-actions">
              <button
                className="button-secondary"
                disabled={submitting}
                onClick={closeDialog}
                type="button"
              >
                取消
              </button>
              <button
                className="button-danger"
                disabled={!confirmationValid || submitting}
                type="submit"
              >
                {submitting ? '正在创建并排队…' : '创建并发布下线 Release'}
              </button>
            </div>
          </form>
        </div>
      ) : null}
    </section>
  )
}
