import { NavLink, Outlet, useLocation } from 'react-router'

import { useConsoleContext } from '../App'
import { PageToolbar } from '../components/PageToolbar'

// Environment-level WAF workspace shell: 策略 / 发布 / 命中统计 / 边缘函数 share
// one contextual sidebar (docs/edge-functions-and-waf-design.md §4.4).
const wafNavigationGroups = [
  {
    group: '配置',
    items: [
      { path: '/waf', label: '策略', detail: '规则组与编译预览', end: true },
      { path: '/waf/edge-functions', label: '边缘函数', detail: '手写脚本草稿', end: false },
    ],
  },
  {
    group: '观察',
    items: [{ path: '/waf/hits', label: '命中统计', detail: 'Prometheus 聚合', end: false }],
  },
  {
    group: '资产与历史',
    items: [{ path: '/waf/releases', label: '发布', detail: '脚本 diff 与 Release', end: false }],
  },
]

const wafTabTitles: Record<string, string> = {
  '': 'WAF 策略',
  releases: 'WAF 发布',
  hits: '命中统计',
  'edge-functions': '边缘函数',
}

export function WafLayout() {
  const consoleContext = useConsoleContext()
  const location = useLocation()
  const activeTab = location.pathname.replace('/waf', '').replace(/^\//, '').split('/')[0] ?? ''

  return (
    <div className="workspace">
      <aside className="context-sidebar">
        <div className="workspace-switcher">
          <h2 className="project-switcher-title">WAF</h2>
          <small>环境级 · 数据面零策略</small>
        </div>

        <nav className="context-navigation" aria-label="WAF 功能">
          {wafNavigationGroups.map((group) => (
            <div className="context-nav-group" key={group.group}>
              <p className="context-nav-group-label">{group.group}</p>
              {group.items.map((item) => (
                <NavLink
                  className={({ isActive }) =>
                    `context-nav-item${isActive ? ' context-nav-item-active' : ''}`
                  }
                  end={item.end}
                  key={item.path}
                  to={item.path}
                >
                  <span>{item.label}</span>
                  <small>{item.detail}</small>
                </NavLink>
              ))}
            </div>
          ))}
        </nav>

        <div className="context-sidebar-footer">
          <span className="context-sidebar-note">策略编译为边缘函数脚本后经 rnacos 下发</span>
        </div>
      </aside>

      <div className="workspace-main">
        <PageToolbar title={wafTabTitles[activeTab] ?? 'WAF'} />

        {consoleContext.statusError ? (
          <div className="error-banner" role="alert">
            <strong>部分信息加载失败</strong>
            <span>{consoleContext.statusError}</span>
          </div>
        ) : null}

        <div className="page-content">
          <Outlet />
        </div>
      </div>
    </div>
  )
}
