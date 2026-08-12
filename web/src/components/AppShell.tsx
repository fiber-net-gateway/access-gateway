import type { ReactNode } from 'react'
import { NavLink } from 'react-router'

import type { ApiConnectionState } from '../api/types'

interface AppShellProps {
  apiState: ApiConnectionState
  children: ReactNode
}

interface NavigationItem {
  label: string
  detail: string
  href?: string
}

const navigation: NavigationItem[] = [
  { label: 'Projects', detail: '域名与路由', href: '/projects' },
  { label: 'Certificates', detail: '自动匹配与版本', href: '/certificates' },
  { label: 'Releases', detail: '版本与 rnacos 发布' },
  { label: 'Audit', detail: '即将开放' },
  { label: 'System', detail: '部署能力' },
]

const connectionLabel: Record<ApiConnectionState, string> = {
  loading: '正在连接 Console API',
  online: 'Console API 已连接',
  offline: 'Console API 连接失败',
}

export function AppShell({ apiState, children }: AppShellProps) {
  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand-block">
          <div className="brand-mark" aria-hidden="true">
            AG
          </div>
          <div>
            <p className="eyebrow">CONTROL PLANE</p>
            <p className="brand-name">Access Gateway</p>
          </div>
        </div>

        <nav className="primary-navigation" aria-label="主导航">
          {navigation.map((item) =>
            item.href ? (
              <NavLink
                className={({ isActive }) =>
                  `navigation-item${isActive ? ' navigation-item-active' : ''}`
                }
                key={item.label}
                to={item.href}
              >
                <span>{item.label}</span>
                <small>{item.detail}</small>
              </NavLink>
            ) : (
              <div className="navigation-item" aria-disabled="true" key={item.label}>
                <span>{item.label}</span>
                <small>{item.detail}</small>
              </div>
            ),
          )}
        </nav>

        <div className={`connection-state connection-state-${apiState}`} role="status">
          <span className="connection-dot" aria-hidden="true" />
          <span>{connectionLabel[apiState]}</span>
        </div>
      </aside>

      <main className="main-content">{children}</main>
    </div>
  )
}
