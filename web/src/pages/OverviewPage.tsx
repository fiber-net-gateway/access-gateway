import type { ApiConnectionState, HealthResponse } from '../api/types'

interface OverviewPageProps {
  apiState: ApiConnectionState
  health: HealthResponse | null
  errorMessage: string | null
}

interface SummaryCard {
  label: string
  value: string
  hint: string
  tone: 'ready' | 'pending' | 'unknown'
}

export function OverviewPage({ apiState, health, errorMessage }: OverviewPageProps) {
  const cards: SummaryCard[] = [
    {
      label: 'Console API',
      value: apiState === 'online' ? '可用' : apiState === 'loading' ? '检查中' : '不可用',
      hint: health ? `${health.service} · v${health.version}` : '等待后端健康检查',
      tone: apiState === 'online' ? 'ready' : apiState === 'loading' ? 'unknown' : 'pending',
    },
    {
      label: '配置数据源',
      value: '未接入',
      hint: '后续接入数据库与 rnacos 环境配置',
      tone: 'pending',
    },
    {
      label: '实例激活状态',
      value: '未知',
      hint: '尚无 access-server 实例级激活证据',
      tone: 'unknown',
    },
  ]

  return (
    <div className="overview-page">
      <header className="page-header">
        <div>
          <p className="eyebrow">ACCESS GATEWAY / 本地开发环境</p>
          <h1>控制面工作台</h1>
          <p className="page-description">
            管理项目路由、灰度规则和不可变发布记录。当前阶段已完成前后端基础联通。
          </p>
        </div>
        <div className="environment-badge">
          <span>环境</span>
          <strong>LOCAL</strong>
        </div>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>Console API 暂不可用</strong>
          <span>{errorMessage}</span>
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

      <section className="foundation-panel">
        <div>
          <p className="eyebrow">FOUNDATION READY</p>
          <h2>基础框架已建立</h2>
          <p>
            Web 使用 React + Vite，API 使用 Node.js + Fastify。开发环境通过同源
            <code>/api</code> 代理联通，后续业务模块可以按领域逐步接入。
          </p>
        </div>
        <ol className="next-step-list">
          <li>
            <span>01</span>
            <div>
              <strong>环境与身份</strong>
              <small>环境隔离、登录会话和权限边界</small>
            </div>
          </li>
          <li>
            <span>02</span>
            <div>
              <strong>项目配置</strong>
              <small>结构化编辑与 native codec 校验</small>
            </div>
          </li>
          <li>
            <span>03</span>
            <div>
              <strong>发布工作流</strong>
              <small>不可变 release、rnacos 写入与证据</small>
            </div>
          </li>
        </ol>
      </section>
    </div>
  )
}
