import { useMemo } from 'react'

import type { ProjectNetworkPolicy } from '../api/types'
import { useProjectContext } from './ProjectLayout'

function lines(values: readonly string[]): string {
  return values.join('\n')
}

function parseLines(value: string): readonly string[] {
  return value
    .split(/\r?\n/u)
    .map((item) => item.trim())
    .filter(Boolean)
}

const utf8Encoder = new TextEncoder()

export function ProjectNetworkPolicyPage() {
  const {
    systemStatus,
    workspace: model,
    setWorkspace: setModel,
    workspaceLoading: loading,
  } = useProjectContext()

  const policy = useMemo<ProjectNetworkPolicy>(
    () => ({
      source: model.networkPolicy.source,
      httpsRedirect: model.networkPolicy.httpsRedirect,
      allowedCidrs: model.networkPolicy.allowedCidrs,
      deniedCidrs: model.networkPolicy.deniedCidrs,
    }),
    [model.networkPolicy],
  )
  const routeLimits = systemStatus?.dependencies.nativeValidator.limits?.projectRoute ?? null
  const cidrs = [...policy.allowedCidrs, ...policy.deniedCidrs]
  const cidrLimitMessage = routeLimits
    ? cidrs.length > routeLimits.maxCidrsPerRoute
      ? `CIDR 合计超过 Native 上限 ${routeLimits.maxCidrsPerRoute}`
      : cidrs.some((cidr) => utf8Encoder.encode(cidr).byteLength > routeLimits.maxCidrBytes)
        ? `单条 CIDR 超过 Native 上限 ${routeLimits.maxCidrBytes} UTF-8 bytes`
        : null
    : null
  return (
    <section className="project-subpage" aria-labelledby="network-policy-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT / NETWORK POLICY</p>
          <h2 id="network-policy-title">Network Policy</h2>
          <p>
            网络访问策略与 Routes 一起保存为不可变配置版本；只有 Release
            发布并经实例证据确认后才算激活。
          </p>
        </div>
        <span className="status-chip status-chip-unknown">由 Project 工作区统一保存</span>
      </header>

      {cidrLimitMessage ? (
        <div className="project-validation-errors" role="alert">
          <p>
            <strong>limit_exceeded</strong>
            <span>{cidrLimitMessage}</span>
          </p>
        </div>
      ) : null}

      <div className="network-policy-form">
        <div className="capability-notice" role="note">
          access-server 默认从 socket peer 获取客户端地址；使用 Ingress/LB 时，必须显式配置可信
          proxy 与地址来源，不能仅依赖未清洗的转发头。
        </div>
        <fieldset disabled={loading}>
          <legend>策略所有权</legend>
          <label className="policy-option">
            <input
              checked={model.networkPolicy.source === 'route'}
              name="network-policy-source"
              type="radio"
              onChange={() =>
                setModel((current) => ({
                  ...current,
                  networkPolicy: { ...current.networkPolicy, source: 'route' },
                }))
              }
            />
            <span>
              <strong>由各 Route 配置</strong>
              <small>保留每条 YAML 中的 allows；项目级 CIDR 不参与编译。</small>
            </span>
          </label>
          <label className="policy-option">
            <input
              checked={model.networkPolicy.source === 'project'}
              name="network-policy-source"
              type="radio"
              onChange={() =>
                setModel((current) => ({
                  ...current,
                  networkPolicy: { ...current.networkPolicy, source: 'project' },
                }))
              }
            />
            <span>
              <strong>Project 统一强制</strong>
              <small>
                确定性注入所有 Route；Route YAML 出现 allows 时校验失败，避免策略被绕过。
              </small>
            </span>
          </label>
        </fieldset>

        <div className="network-cidr-grid">
          <label>
            允许 CIDR（每行一项）
            <textarea
              disabled={model.networkPolicy.source !== 'project' || loading}
              placeholder={'10.0.0.0/8\n2001:db8::/32'}
              rows={9}
              spellCheck={false}
              value={lines(model.networkPolicy.allowedCidrs)}
              onChange={(event) =>
                setModel((current) => ({
                  ...current,
                  networkPolicy: {
                    ...current.networkPolicy,
                    allowedCidrs: parseLines(event.target.value),
                  },
                }))
              }
            />
            <small>
              非空时，请求源地址必须匹配至少一项。当前 {policy.allowedCidrs.length} 条。
            </small>
          </label>
          <label>
            拒绝 CIDR（每行一项）
            <textarea
              disabled={model.networkPolicy.source !== 'project' || loading}
              placeholder={'10.1.0.0/16\n2001:db8:dead::/48'}
              rows={9}
              spellCheck={false}
              value={lines(model.networkPolicy.deniedCidrs)}
              onChange={(event) =>
                setModel((current) => ({
                  ...current,
                  networkPolicy: {
                    ...current.networkPolicy,
                    deniedCidrs: parseLines(event.target.value),
                  },
                }))
              }
            />
            <small>
              拒绝规则优先；发布时编译为 native `!CIDR` 形式。当前 {policy.deniedCidrs.length} 条。
            </small>
          </label>
        </div>

        <div className="form-actions">
          <span>修改已进入 Project Working Copy；请使用页面顶部的 Project 级“保存为版本”。</span>
        </div>
      </div>
    </section>
  )
}
