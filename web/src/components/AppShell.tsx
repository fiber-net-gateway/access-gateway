import type { ReactNode } from 'react'

import type { ApiConnectionState } from '../api/types'

interface AppShellProps {
  apiState: ApiConnectionState
  children: ReactNode
}

interface NavigationItem {
  label: string
  detail: string
  active?: boolean
}

const navigation: NavigationItem[] = [
  { label: '运行总览', detail: 'Overview', active: true },
  { label: '项目与路由', detail: '即将开放' },
  { label: '灰度规则', detail: '即将开放' },
  { label: '发布中心', detail: '即将开放' },
  { label: '运行实例', detail: '即将开放' },
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
          {navigation.map((item) => (
            <div
              className={`navigation-item${item.active ? ' navigation-item-active' : ''}`}
              aria-current={item.active ? 'page' : undefined}
              aria-disabled={item.active ? undefined : true}
              key={item.label}
            >
              <span>{item.label}</span>
              <small>{item.detail}</small>
            </div>
          ))}
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
