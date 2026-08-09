import { useState, type FormEvent } from 'react'

import type {
  ApiConnectionState,
  CreateEnvironmentInput,
  EnvironmentView,
  HealthResponse,
  ProjectView,
  SystemStatusResponse,
} from '../api/types'

interface OverviewPageProps {
  apiState: ApiConnectionState
  health: HealthResponse | null
  systemStatus: SystemStatusResponse | null
  environments: readonly EnvironmentView[]
  selectedEnvironmentId: string | null
  projects: readonly ProjectView[]
  errorMessage: string | null
  onSelectEnvironment(id: string): void
  onCreateEnvironment(input: CreateEnvironmentInput): Promise<void>
  onCreateProject(name: string): Promise<void>
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
    environments,
    selectedEnvironmentId,
    projects,
    errorMessage,
    onSelectEnvironment,
    onCreateEnvironment,
    onCreateProject,
  } = props
  const [environmentForm, setEnvironmentForm] = useState<CreateEnvironmentInput>({
    code: '',
    name: '',
    tier: 'local',
    nacosEndpoint: 'http://127.0.0.1:8848',
  })
  const [projectName, setProjectName] = useState('')
  const [actionError, setActionError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)
  const selectedEnvironment = environments.find((item) => item.id === selectedEnvironmentId)

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

  const submitEnvironment = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setSaving(true)
    setActionError(null)
    try {
      await onCreateEnvironment(environmentForm)
      setEnvironmentForm((current) => ({ ...current, code: '', name: '' }))
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '创建环境失败')
    } finally {
      setSaving(false)
    }
  }

  const submitProject = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setSaving(true)
    setActionError(null)
    try {
      await onCreateProject(projectName)
      setProjectName('')
    } catch (error) {
      setActionError(error instanceof Error ? error.message : '创建项目失败')
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="overview-page">
      <header className="page-header">
        <div>
          <p className="eyebrow">ACCESS GATEWAY / CONTROL PLANE</p>
          <h1>环境与项目</h1>
          <p className="page-description">
            管理环境隔离、项目路由草稿和发布前能力。数据库、校验器和 Worker 的状态来自后端真实探测。
          </p>
        </div>
        <div className="environment-badge">
          <span>环境</span>
          <strong>{selectedEnvironment?.code.toUpperCase() ?? 'NONE'}</strong>
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
              <p className="eyebrow">ENVIRONMENTS</p>
              <h2>环境</h2>
            </div>
            <span>{environments.length}</span>
          </div>
          <div className="entity-list">
            {environments.map((environment) => (
              <button
                className={
                  environment.id === selectedEnvironmentId
                    ? 'entity-row entity-row-active'
                    : 'entity-row'
                }
                key={environment.id}
                onClick={() => onSelectEnvironment(environment.id)}
                type="button"
              >
                <span>
                  <strong>{environment.name}</strong>
                  <small>{environment.nacos.endpoint}</small>
                </span>
                <code>{environment.code}</code>
              </button>
            ))}
            {environments.length === 0 ? <p className="empty-state">尚未创建环境。</p> : null}
          </div>
          <form className="compact-form" onSubmit={(event) => void submitEnvironment(event)}>
            <h3>新建环境</h3>
            <div className="form-row">
              <label>
                Code
                <input
                  required
                  value={environmentForm.code}
                  onChange={(event) =>
                    setEnvironmentForm((current) => ({ ...current, code: event.target.value }))
                  }
                />
              </label>
              <label>
                名称
                <input
                  required
                  value={environmentForm.name}
                  onChange={(event) =>
                    setEnvironmentForm((current) => ({ ...current, name: event.target.value }))
                  }
                />
              </label>
            </div>
            <label>
              Nacos Endpoint
              <input
                required
                type="url"
                value={environmentForm.nacosEndpoint}
                onChange={(event) =>
                  setEnvironmentForm((current) => ({
                    ...current,
                    nacosEndpoint: event.target.value,
                  }))
                }
              />
            </label>
            <button
              disabled={saving || systemStatus?.dependencies.database.status !== 'ready'}
              type="submit"
            >
              创建环境
            </button>
          </form>
        </article>

        <article className="workspace-panel">
          <div className="panel-heading">
            <div>
              <p className="eyebrow">PROJECT ROUTES</p>
              <h2>{selectedEnvironment?.name ?? '项目'}</h2>
            </div>
            <span>{projects.length}</span>
          </div>
          <div className="entity-list">
            {projects.map((project) => (
              <div className="entity-row" key={project.id}>
                <span>
                  <strong>{project.name}</strong>
                  <small>
                    {project.draft
                      ? `草稿 r${project.draft.revision} · ${project.draft.state}`
                      : '尚无活动草稿'}
                  </small>
                </span>
                <span className="status-chip status-chip-unknown">激活未知</span>
              </div>
            ))}
            {selectedEnvironmentId && projects.length === 0 ? (
              <p className="empty-state">这个环境还没有项目。</p>
            ) : null}
          </div>
          <form className="compact-form" onSubmit={(event) => void submitProject(event)}>
            <h3>新建项目</h3>
            <label>
              项目名
              <input
                required
                value={projectName}
                onChange={(event) => setProjectName(event.target.value)}
              />
            </label>
            <button disabled={saving || !selectedEnvironmentId} type="submit">
              创建项目
            </button>
          </form>
        </article>
      </section>
    </div>
  )
}
