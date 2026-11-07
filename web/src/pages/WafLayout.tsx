import { NavLink, Outlet } from 'react-router'

import { useConsoleContext } from '../App'
import { CapabilityStrip } from '../components/CapabilityStrip'

// Environment-level WAF workspace shell: 策略 / 发布 / 边缘函数 share one
// navigation rail (docs/edge-functions-and-waf-design.md §4.4).
const wafNavigation = [
  { path: '/waf', label: '策略', detail: '规则与编译预览', end: true },
  { path: '/waf/releases', label: '发布', detail: '脚本 diff 与 Release', end: false },
  { path: '/waf/hits', label: '命中统计', detail: 'Prometheus 聚合', end: false },
  { path: '/waf/edge-functions', label: '边缘函数', detail: '当前索引只读', end: false },
]

export function WafLayout() {
  const consoleContext = useConsoleContext()

  return (
    <div className="projects-page waf-page">
      <header className="project-detail-header">
        <div>
          <p className="eyebrow">WAF EDGE FUNCTIONS</p>
          <h1>WAF</h1>
          <div className="project-status-row">
            <span>策略编译为边缘函数脚本后经 rnacos 下发</span>
            <span>数据面零策略逻辑，仅执行生成脚本</span>
          </div>
        </div>
      </header>

      {consoleContext.statusError ? (
        <div className="error-banner" role="alert">
          <strong>部分信息加载失败</strong>
          <span>{consoleContext.statusError}</span>
        </div>
      ) : null}

      <CapabilityStrip
        apiState={consoleContext.apiState}
        health={consoleContext.health}
        systemStatus={consoleContext.systemStatus}
      />

      <nav className="project-tabs" aria-label="WAF 功能">
        {wafNavigation.map((item) => (
          <NavLink
            className={({ isActive }) => `project-tab${isActive ? ' project-tab-active' : ''}`}
            key={item.path}
            to={item.path}
            end={item.end}
          >
            <strong>{item.label}</strong>
            <small>{item.detail}</small>
          </NavLink>
        ))}
      </nav>

      <div className="project-page-content">
        <Outlet />
      </div>
    </div>
  )
}
