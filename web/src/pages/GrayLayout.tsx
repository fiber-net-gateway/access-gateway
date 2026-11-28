import { NavLink, Outlet, useLocation } from 'react-router'

import { useConsoleContext } from '../App'
import { PageToolbar } from '../components/PageToolbar'

const grayNavigationGroups = [
  {
    group: '配置',
    items: [{ path: '/gray', label: '灰度规则', detail: '入口网络、万分比与 CIDR', end: true }],
  },
  {
    group: '资产与历史',
    items: [
      {
        path: '/gray/releases',
        label: '发布与回滚',
        detail: '不可变 Release 与激活证据',
        end: false,
      },
    ],
  },
]

const grayTabTitles: Record<string, string> = {
  '': '灰度规则',
  releases: '灰度发布',
}

export function GrayLayout() {
  const consoleContext = useConsoleContext()
  const location = useLocation()
  const activeTab = location.pathname.replace('/gray', '').replace(/^\//, '').split('/')[0] ?? ''

  return (
    <div className="workspace">
      <aside className="context-sidebar">
        <div className="workspace-switcher">
          <h2 className="project-switcher-title">灰度路由</h2>
          <small>环境级 · ploto.unified-access.gray-match</small>
        </div>

        <nav className="context-navigation" aria-label="灰度功能">
          {grayNavigationGroups.map((group) => (
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
          <span className="context-sidebar-note">
            经 Native Validator 校验后发布至 rnacos（DEFAULT_GROUP），实例激活需以激活证据为准
          </span>
        </div>
      </aside>

      <div className="workspace-main">
        <PageToolbar title={grayTabTitles[activeTab] ?? '灰度路由'} />

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
