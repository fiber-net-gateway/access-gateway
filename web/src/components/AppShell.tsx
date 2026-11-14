import type { ReactNode } from 'react'
import { Link, NavLink } from 'react-router'

import type { ApiConnectionState, HealthResponse, SystemStatusResponse } from '../api/types'

interface AppShellProps {
  apiState: ApiConnectionState
  health: HealthResponse | null
  systemStatus: SystemStatusResponse | null
  children: ReactNode
}

const primaryNavigation = [
  { to: '/overview', label: '概览', end: true },
  { to: '/projects', label: '项目', end: false },
  { to: '/waf', label: 'WAF', end: false },
  { to: '/certificates', label: '证书', end: false },
  { to: '/docs', label: '文档', end: false },
]

function dependencyTone(status: string | undefined): 'ready' | 'pending' | 'unknown' {
  if (status === 'ready') return 'ready'
  if (status === 'unavailable') return 'pending'
  return 'unknown'
}

export function AppShell({ apiState, health, systemStatus, children }: AppShellProps) {
  const validator = systemStatus?.dependencies.nativeValidator
  const publicationWorker = systemStatus?.dependencies.publicationWorker
  const database = systemStatus?.dependencies.database
  const healthLabel =
    apiState === 'online'
      ? `正常${health?.version ? ` · v${health.version}` : ''}`
      : apiState === 'loading'
        ? '连接中'
        : '连接失败'

  return (
    <div className="app-frame">
      <header className="top-bar">
        <Link className="brand" to="/overview">
          <span className="brand-mark" aria-hidden="true">
            AG
          </span>
          <span className="brand-name">Access Gateway</span>
        </Link>

        <nav className="top-navigation" aria-label="全局导航">
          {primaryNavigation.map((item) => (
            <NavLink
              className={({ isActive }) => `top-nav-item${isActive ? ' top-nav-item-active' : ''}`}
              end={item.end}
              key={item.to}
              to={item.to}
            >
              {item.label}
            </NavLink>
          ))}
        </nav>

        <Link
          className={`health-pill health-pill-${apiState}`}
          title="系统连接状态,点击查看概览"
          to="/overview"
        >
          <span className={`connection-dot connection-dot-${apiState}`} aria-hidden="true" />
          <span>{healthLabel}</span>
        </Link>
      </header>

      <main className="app-main">{children}</main>

      <footer className="status-footer" role="status">
        <Link className="status-footer-link" to="/overview">
          <span className={`connection-dot connection-dot-${apiState}`} aria-hidden="true" />
          <span>Console API</span>
        </Link>
        <Link className="status-footer-link" to="/overview">
          <span>Native Validator</span>
          <span className={`footer-tone footer-tone-${dependencyTone(validator?.status)}`}>
            {validator?.status === 'ready' ? '可用' : validator?.status ? '不可用' : '未知'}
          </span>
        </Link>
        <Link className="status-footer-link" to="/overview">
          <span>rnacos 发布</span>
          <span className={`footer-tone footer-tone-${dependencyTone(publicationWorker?.status)}`}>
            {publicationWorker?.status === 'ready'
              ? '可用'
              : publicationWorker?.status
                ? '不可用'
                : '未知'}
          </span>
        </Link>
        <Link className="status-footer-link" to="/overview">
          <span>MySQL</span>
          <span className={`footer-tone footer-tone-${dependencyTone(database?.status)}`}>
            {database?.status === 'ready' ? '就绪' : database?.status ? '异常' : '未知'}
          </span>
        </Link>
      </footer>
    </div>
  )
}
