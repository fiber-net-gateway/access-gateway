import { useState, type FormEvent } from 'react'

import type {
  ApiConnectionState,
  EnvironmentView,
  HealthResponse,
  ProjectView,
  SystemStatusResponse,
} from '../api/types'

interface OverviewPageProps {
  apiState: ApiConnectionState
  health: HealthResponse | null
  systemStatus: SystemStatusResponse | null
  workspace: EnvironmentView | null
  projects: readonly ProjectView[]
  selectedProjectId: string | null
  routeDocument: string
  routeLoading: boolean
  hasUnsavedChanges: boolean
  errorMessage: string | null
  onSelectProject(id: string): void
  onCreateProject(domain: string): Promise<void>
  onRouteDocumentChange(value: string): void
  onSaveRoutes(): Promise<void>
}

interface SummaryCard {
  label: string
  value: string
  hint: string
  tone: 'ready' | 'pending' | 'unknown'
}

function capabilityCard(
  label: string,
  capability: { status: string; detail: string } | undefined,
): SummaryCard {
  return {
    label,
    value:
      capability?.status === 'ready'
        ? '可用'
        : capability?.status === 'unavailable'
          ? '不可用'
          : '未配置',
    hint: capability?.detail ?? '正在获取能力状态',
    tone:
      capability?.status === 'ready'
        ? 'ready'
        : capability?.status === 'unavailable'
          ? 'pending'
          : 'unknown',
  }
}

export function OverviewPage(props: OverviewPageProps) {
  const {
    apiState,
    health,
    systemStatus,
    workspace,
    projects,
    selectedProjectId,
    routeDocument,
    routeLoading,
    hasUnsavedChanges,
    errorMessage,
    onSelectProject,
    onCreateProject,
    onRouteDocumentChange,
    onSaveRoutes,
  } = props
  const [domain, setDomain] = useState('')
  const [actionError, setActionError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)
  const selectedProject = projects.find((item) => item.id === selectedProjectId) ?? null

  const cards: SummaryCard[] = [
    {
      label: 'Console API',
      value: apiState === 'online' ? '可用' : apiState === 'loading' ? '检查中' : '不可用',
      hint: health ? `${health.service} · v${health.version}` : '等待后端健康检查',
      tone: apiState === 'online' ? 'ready' : apiState === 'loading' ? 'unknown' : 'pending',
    },
    capabilityCard('MySQL / Schema', systemStatus?.dependencies.database),
    capabilityCard('Native Validator', systemStatus?.dependencies.nativeValidator),
  ]

  const submitProject = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setSaving(true)
    setActionError(null)
    try {
      await onCreateProject(domain)
      setDomain('')
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '创建域名失败')
    } finally {
      setSaving(false)
    }
  }

  const saveRoutes = async (): Promise<void> => {
    setSaving(true)
    setActionError(null)
    try {
      await onSaveRoutes()
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '保存路由失败')
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="overview-page">
      <header className="page-header">
        <div>
          <p className="eyebrow">ACCESS GATEWAY / CONTROL PLANE</p>
          <h1>域名与路由</h1>
          <p className="page-description">
            当前部署使用一个固定工作区。创建域名项目后，在同一页面维护完整的 Host 与 Route 草稿。
          </p>
        </div>
        <div className="environment-badge">
          <span>固定工作区</span>
          <strong>{workspace?.code.toUpperCase() ?? 'LOADING'}</strong>
        </div>
      </header>

      {errorMessage || actionError ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{actionError ?? errorMessage}</span>
        </div>
      ) : null}

      <section className="summary-grid" aria-label="系统状态">
        {cards.map((card) => (
          <article className="summary-card" key={card.label}>
            <div className="summary-card-heading">
              <span>{card.label}</span>
              <span className={`status-chip status-chip-${card.tone}`}>{card.value}</span>
            </div>
            <p>{card.hint}</p>
          </article>
        ))}
      </section>

      <section className="workspace-grid">
        <article className="workspace-panel">
          <div className="panel-heading">
            <div>
              <p className="eyebrow">FIXED WORKSPACE</p>
              <h2>{workspace?.name ?? '工作区加载中'}</h2>
            </div>
            <span>1</span>
          </div>
          <dl className="workspace-details">
            <div>
              <dt>Nacos</dt>
              <dd>{workspace?.nacos.endpoint ?? '—'}</dd>
            </div>
            <div>
              <dt>Namespace</dt>
              <dd>{workspace?.nacos.namespace || 'public'}</dd>
            </div>
            <div>
              <dt>Zone</dt>
              <dd>{workspace?.code ?? '—'}</dd>
            </div>
          </dl>
          <p className="panel-note">工作区由部署初始化，不在 Console 中创建或切换。</p>
        </article>

        <article className="workspace-panel">
          <div className="panel-heading">
            <div>
              <p className="eyebrow">DOMAIN PROJECTS</p>
              <h2>域名项目</h2>
            </div>
            <span>{projects.length}</span>
          </div>
          <div className="entity-list">
            {projects.map((project) => (
              <button
                className={
                  project.id === selectedProjectId ? 'entity-row entity-row-active' : 'entity-row'
                }
                key={project.id}
                onClick={() => onSelectProject(project.id)}
                type="button"
              >
                <span>
                  <strong>{project.domain}</strong>
                  <small>
                    {project.draft
                      ? `草稿 r${project.draft.revision} · ${project.draft.state}`
                      : '尚无活动草稿'}
                  </small>
                </span>
                <span className="status-chip status-chip-unknown">未发布</span>
              </button>
            ))}
            {projects.length === 0 ? <p className="empty-state">还没有域名项目。</p> : null}
          </div>
          <form className="compact-form" onSubmit={(event) => void submitProject(event)}>
            <h3>添加域名</h3>
            <label>
              域名
              <input
                autoComplete="off"
                placeholder="api.example.com"
                required
                value={domain}
                onChange={(event) => setDomain(event.target.value)}
              />
            </label>
            <button disabled={saving || !workspace} type="submit">
              创建域名项目
            </button>
          </form>
        </article>

        <article className="workspace-panel route-editor-panel">
          <div className="panel-heading">
            <div>
              <p className="eyebrow">PROJECT ROUTE DRAFT</p>
              <h2>{selectedProject?.domain ?? '选择域名后配置路由'}</h2>
            </div>
            <span>{selectedProject?.draft ? `r${selectedProject.draft.revision}` : '—'}</span>
          </div>
          <p className="panel-note">
            编辑完整 project_route JSON。保存只创建 MySQL 草稿修订，不表示已经发布到 R-Nacos。
          </p>
          <textarea
            aria-label="路由 JSON"
            className="route-editor"
            disabled={!selectedProject || routeLoading || saving}
            onChange={(event) => onRouteDocumentChange(event.target.value)}
            spellCheck={false}
            value={routeLoading ? '正在加载路由草稿…' : routeDocument}
          />
          <div className="route-editor-actions">
            <span>
              支持 RESPONSE、PROXY、condition、rewrite、headers、CIDR 与 body limit。
              <span
                aria-live="polite"
                className={`status-chip status-chip-${hasUnsavedChanges ? 'pending' : 'ready'}`}
              >
                {hasUnsavedChanges ? '有未保存修改' : '草稿已保存'}
              </span>
            </span>
            <button
              disabled={!selectedProject?.draft || routeLoading || saving}
              onClick={() => void saveRoutes()}
              type="button"
            >
              保存草稿修订
            </button>
          </div>
        </article>
      </section>
    </div>
  )
}
