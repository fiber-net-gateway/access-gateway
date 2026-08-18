import { useCallback, useEffect, useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router'

import {
  createRelease,
  fetchConfigurationVersion,
  fetchConfigurationVersions,
  queueReleasePublication,
  restoreConfigurationVersion,
} from '../api/client'
import type { ConfigurationVersionDetail, ConfigurationVersionSummary } from '../api/types'
import { useProjectContext } from './ProjectLayout'

export function ProjectVersionsPage() {
  const { project, refreshProject, systemStatus } = useProjectContext()
  const navigate = useNavigate()
  const [versions, setVersions] = useState<readonly ConfigurationVersionSummary[]>([])
  const [currentVersionId, setCurrentVersionId] = useState<string | null>(null)
  const [configurationLockVersion, setConfigurationLockVersion] = useState('0')
  const [previewVersion, setPreviewVersion] = useState<ConfigurationVersionDetail | null>(null)
  const [editSourceVersion, setEditSourceVersion] = useState<ConfigurationVersionSummary | null>(
    null,
  )
  const [loading, setLoading] = useState(true)
  const [restoring, setRestoring] = useState(false)
  const [publishing, setPublishing] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [publishDialogOpen, setPublishDialogOpen] = useState(false)
  const [publishVersionId, setPublishVersionId] = useState('')
  const [releaseTitle, setReleaseTitle] = useState('')
  const [releaseDescription, setReleaseDescription] = useState('')
  const publicationReady = systemStatus?.dependencies.publicationWorker.status === 'ready'

  const loadVersions = useCallback(
    async (signal?: AbortSignal): Promise<void> => {
      setLoading(true)
      try {
        const result = await fetchConfigurationVersions(project.id, signal)
        setVersions(result.items)
        setCurrentVersionId(result.currentVersionId)
        setConfigurationLockVersion(result.lockVersion)
        setErrorMessage(null)
      } finally {
        setLoading(false)
      }
    },
    [project.id],
  )

  useEffect(() => {
    const controller = new AbortController()
    void loadVersions(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载配置版本失败')
      }
    })
    return () => controller.abort()
  }, [loadVersions])

  const runAction = async (action: () => Promise<void>): Promise<void> => {
    setErrorMessage(null)
    try {
      await action()
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '操作失败')
    }
  }

  const viewVersion = async (versionId: string): Promise<void> => {
    setPreviewVersion(await fetchConfigurationVersion(project.id, versionId))
  }

  const openEditSourceDialog = (version: ConfigurationVersionSummary): void => {
    if (version.relation === 'current') {
      void navigate('../routes')
      return
    }
    setPreviewVersion(null)
    setEditSourceVersion(version)
  }

  const beginEditingFromVersion = async (): Promise<void> => {
    if (!editSourceVersion) return
    await navigate(`../routes?sourceVersionId=${encodeURIComponent(editSourceVersion.id)}`)
  }

  const restoreVersion = async (version: ConfigurationVersionSummary): Promise<void> => {
    if (!currentVersionId) throw new Error('当前项目还没有可恢复的配置版本')
    if (!window.confirm(`将 V${version.number} 的内容恢复为新的当前版本，是否继续？`)) return
    setRestoring(true)
    try {
      await restoreConfigurationVersion(
        project.id,
        version.id,
        currentVersionId,
        configurationLockVersion,
        `从 V${version.number} 恢复`,
      )
      setPreviewVersion(null)
      await Promise.all([loadVersions(), refreshProject()])
    } finally {
      setRestoring(false)
    }
  }

  const openPublishDialog = (version: ConfigurationVersionSummary): void => {
    setPreviewVersion(null)
    setPublishVersionId(version.id)
    setReleaseTitle(`发布 V${version.number} · ${project.domain}`)
    setReleaseDescription(version.changeSummary)
    setPublishDialogOpen(true)
  }

  const submitRelease = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    await runAction(async () => {
      if (!currentVersionId) throw new Error('请先保存一个配置版本')
      setPublishing(true)
      try {
        const prepared = await createRelease(
          project.id,
          publishVersionId,
          currentVersionId,
          releaseTitle,
          releaseDescription,
        )
        await queueReleasePublication(prepared.id)
        setPublishDialogOpen(false)
        await refreshProject()
        await navigate('../releases')
      } finally {
        setPublishing(false)
      }
    })
  }

  const selectedPublishVersion = versions.find((version) => version.id === publishVersionId) ?? null
  const currentVersion = versions.find((version) => version.id === currentVersionId) ?? null
  const restoredFromLabel = (version: ConfigurationVersionSummary): string | null => {
    if (!version.restoredFromVersionId) return null
    const source = versions.find((item) => item.id === version.restoredFromVersionId)
    return source ? `基于 V${source.number}` : '基于历史版本'
  }

  return (
    <section className="project-subpage" aria-labelledby="versions-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">IMMUTABLE CONFIGURATION</p>
          <h2 id="versions-title">配置版本</h2>
          <p>历史版本可以作为编辑起点；只有保存时才会创建新的不可变版本。</p>
        </div>
        <span className="status-chip status-chip-unknown">
          {versions.length} 个版本 · ETag {configurationLockVersion}
        </span>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      {loading ? (
        <div className="route-empty-state">正在加载配置版本…</div>
      ) : versions.length === 0 ? (
        <div className="route-empty-state">
          <h3>还没有配置版本</h3>
          <p>前往 Routes 编辑并保存后，系统会创建第一个不可变版本 V1。</p>
        </div>
      ) : (
        <section className="history-panel" aria-label="配置版本列表">
          <ol className="version-list">
            {versions.map((version) => (
              <li key={version.id}>
                <div className="version-number">
                  <strong>V{version.number}</strong>
                  <span>{version.relation === 'current' ? '当前' : '历史'}</span>
                </div>
                <div className="version-summary">
                  <strong>{version.changeSummary}</strong>
                  <small>
                    {version.routeCount} Routes · {version.createdBy.displayName} ·{' '}
                    {new Date(version.createdAt).toLocaleString('zh-CN')}
                    {restoredFromLabel(version) ? ` · ${restoredFromLabel(version)}` : ''}
                  </small>
                </div>
                <div className="version-state">
                  <span className="status-chip status-chip-unknown">
                    {version.publicationStatus === 'never' ? '未发布' : version.publicationStatus}
                  </span>
                </div>
                <div className="version-actions">
                  <button
                    className="button-secondary"
                    onClick={() => void runAction(() => viewVersion(version.id))}
                    type="button"
                  >
                    查看
                  </button>
                  <button
                    className="button-primary"
                    onClick={() => openEditSourceDialog(version)}
                    type="button"
                  >
                    {version.relation === 'current' ? '编辑当前' : '基于此版本编辑'}
                  </button>
                  <button
                    className="button-secondary"
                    disabled={publishing || !publicationReady}
                    onClick={() => openPublishDialog(version)}
                    type="button"
                  >
                    发布
                  </button>
                  {version.relation === 'historical' ? (
                    <button
                      className="button-secondary"
                      disabled={restoring}
                      onClick={() => void runAction(() => restoreVersion(version))}
                      type="button"
                    >
                      原样恢复
                    </button>
                  ) : null}
                </div>
              </li>
            ))}
          </ol>
        </section>
      )}

      {!publicationReady ? (
        <p className="capability-notice">
          rnacos Publication Worker 当前不可用，版本仍可查看和恢复。
        </p>
      ) : null}

      {editSourceVersion ? (
        <div className="dialog-backdrop" role="presentation">
          <section aria-labelledby="edit-from-version-title" className="dialog-card" role="dialog">
            <p className="eyebrow">HISTORICAL EDITING SOURCE</p>
            <h2 id="edit-from-version-title">以 V{editSourceVersion.number} 为起点编辑</h2>
            <div className="version-source-summary">
              <div>
                <span>编辑来源</span>
                <strong>历史 V{editSourceVersion.number}</strong>
              </div>
              <div>
                <span>当前配置</span>
                <strong>{currentVersion ? `V${currentVersion.number}` : '尚无版本'}</strong>
              </div>
              <div>
                <span>保存结果</span>
                <strong>{currentVersion ? `新建 V${currentVersion.number + 1}` : '新建 V1'}</strong>
              </div>
            </div>
            <p>
              进入 Routes 后只会创建本地编辑副本；V{editSourceVersion.number}{' '}
              和当前版本都不会被修改。 完成编辑并保存时，系统才会生成一个新版本。
            </p>
            <div className="dialog-actions">
              <button
                className="button-secondary"
                onClick={() => setEditSourceVersion(null)}
                type="button"
              >
                取消
              </button>
              <button
                className="button-primary"
                onClick={() => void beginEditingFromVersion()}
                type="button"
              >
                进入 Routes 编辑
              </button>
            </div>
          </section>
        </div>
      ) : null}

      {publishDialogOpen ? (
        <div className="dialog-backdrop" role="presentation">
          <form
            aria-labelledby="publish-version-title"
            className="dialog-card"
            onSubmit={(event) => void submitRelease(event)}
            role="dialog"
          >
            <p className="eyebrow">CREATE & PUBLISH RELEASE</p>
            <h2 id="publish-version-title">发布配置版本</h2>
            {selectedPublishVersion?.relation === 'historical' ? (
              <div className="dialog-warning">
                将发布历史版本 V{selectedPublishVersion.number}；当前配置仍为
                {versions.find((version) => version.id === currentVersionId)?.number
                  ? ` V${versions.find((version) => version.id === currentVersionId)!.number}`
                  : '当前版本'}
                。
              </div>
            ) : null}
            <label>
              发布版本
              <select
                required
                value={publishVersionId}
                onChange={(event) => setPublishVersionId(event.target.value)}
              >
                {versions.map((version) => (
                  <option key={version.id} value={version.id}>
                    V{version.number} · {version.relation === 'current' ? '当前' : '历史'} ·{' '}
                    {version.changeSummary}
                  </option>
                ))}
              </select>
            </label>
            <label>
              Release 标题
              <input
                maxLength={255}
                required
                value={releaseTitle}
                onChange={(event) => setReleaseTitle(event.target.value)}
              />
            </label>
            <label>
              说明
              <textarea
                maxLength={4000}
                rows={3}
                value={releaseDescription}
                onChange={(event) => setReleaseDescription(event.target.value)}
              />
            </label>
            <p className="dialog-footnote">
              创建 Release 时会分配新的 native wire version，并使用当前 Native Validator 重新校验。
            </p>
            <div className="dialog-actions">
              <button
                className="button-secondary"
                disabled={publishing}
                onClick={() => setPublishDialogOpen(false)}
                type="button"
              >
                取消
              </button>
              <button className="button-primary" disabled={publishing} type="submit">
                {publishing ? '正在进入发布队列…' : '创建并发布'}
              </button>
            </div>
          </form>
        </div>
      ) : null}

      {previewVersion ? (
        <div className="dialog-backdrop" role="presentation">
          <section
            aria-labelledby="preview-version-title"
            className="dialog-card dialog-card-wide"
            role="dialog"
          >
            <p className="eyebrow">READ-ONLY SNAPSHOT</p>
            <h2 id="preview-version-title">V{previewVersion.number} · 配置快照</h2>
            <p>{previewVersion.changeSummary}</p>
            <section className="version-preview-network" aria-label="Host 与网络策略快照">
              <strong>Host bindings · Network Policy</strong>
              <small>主域名：{project.domain}</small>
              <small>关联域名：{previewVersion.model.hostAliases.join(', ') || '无'}</small>
              <span>
                {previewVersion.model.networkPolicy.source === 'project'
                  ? 'Project 统一策略'
                  : '由各 Route 配置'}
              </span>
              <small>
                HTTPS：
                {previewVersion.model.networkPolicy.httpsRedirect === 'off'
                  ? '不强制'
                  : `${previewVersion.model.networkPolicy.httpsRedirect} 重定向`}
              </small>
              {previewVersion.model.networkPolicy.source === 'project' ? (
                <small>
                  允许：{previewVersion.model.networkPolicy.allowedCidrs.join(', ') || '全部'} ·
                  拒绝：{previewVersion.model.networkPolicy.deniedCidrs.join(', ') || '无'}
                </small>
              ) : null}
            </section>
            <div className="version-preview-routes">
              {previewVersion.model.routes.map((route, index) => (
                <article key={route.id}>
                  <strong>
                    Route {index + 1} · {route.format.toUpperCase()}
                    {route.format === 'js'
                      ? ` · ${route.method ?? 'ALL METHODS'} ${route.path}`
                      : ''}
                  </strong>
                  <pre>{route.source}</pre>
                </article>
              ))}
            </div>
            <div className="dialog-actions">
              <button
                className="button-secondary"
                onClick={() => setPreviewVersion(null)}
                type="button"
              >
                关闭
              </button>
              <button
                className="button-primary"
                onClick={() => openEditSourceDialog(previewVersion)}
                type="button"
              >
                {previewVersion.relation === 'current' ? '编辑当前版本' : '基于此版本编辑'}
              </button>
              <button
                className="button-secondary"
                disabled={publishing || !publicationReady}
                onClick={() => openPublishDialog(previewVersion)}
                type="button"
              >
                发布这个版本
              </button>
            </div>
          </section>
        </div>
      ) : null}
    </section>
  )
}
