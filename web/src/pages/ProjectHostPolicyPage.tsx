import { useMemo, useState } from 'react'

import type { HttpsRedirect } from '../api/types'
import { normalizeExactHost, validateHostAliases } from '../routes/model'
import { useProjectContext } from './ProjectLayout'

export function ProjectHostPolicyPage() {
  const {
    project,
    systemStatus,
    workspace: model,
    setWorkspace: setModel,
    workspaceLoading: loading,
  } = useProjectContext()
  const [hostAliasInput, setHostAliasInput] = useState('')
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const routeLimits = systemStatus?.dependencies.nativeValidator.limits?.projectRoute ?? null
  const hostAliasChecks = useMemo(
    () =>
      validateHostAliases(model.hostAliases, project.domain, {
        maxHosts: routeLimits?.maxHosts,
        maxHostPatternBytes: routeLimits?.maxHostPatternBytes,
      }),
    [model.hostAliases, project.domain, routeLimits?.maxHostPatternBytes, routeLimits?.maxHosts],
  )
  const hostAliasIssues = [...hostAliasChecks.validationIssues, ...hostAliasChecks.limitMessages]
  const updateHostAliases = (hostAliases: readonly string[]): void => {
    setModel((current) => ({ ...current, hostAliases }))
    setErrorMessage(null)
  }

  const addHostAlias = (): void => {
    const normalized = normalizeExactHost(hostAliasInput)
    if (!normalized) {
      setErrorMessage('请输入有效的精确 DNS 域名，不支持通配符、IP、端口或路径。')
      return
    }
    if (normalized === normalizeExactHost(project.domain)) {
      setErrorMessage('关联域名不能与主域名重复。')
      return
    }
    if (model.hostAliases.includes(normalized)) {
      setErrorMessage('该关联域名已存在。')
      return
    }
    if (routeLimits && model.hostAliases.length + 1 >= routeLimits.maxHosts) {
      setErrorMessage(`域名总数已达到 Native 上限 ${routeLimits.maxHosts}。`)
      return
    }
    updateHostAliases([...model.hostAliases, normalized])
    setHostAliasInput('')
  }

  const removeHostAlias = (alias: string): void => {
    updateHostAliases(model.hostAliases.filter((item) => item !== alias))
  }

  return (
    <section className="project-subpage" aria-labelledby="host-policy-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT / HOST POLICY</p>
          <h2 id="host-policy-title">Host Policy</h2>
          <p>
            主域名、额外域名和 HTTPS 入口策略与 Routes 一起保存为不可变配置版本；只有 Release
            发布并经实例证据确认后才算激活。
          </p>
        </div>
        <span className="status-chip status-chip-unknown">由 Project 工作区统一保存</span>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      {hostAliasIssues.length > 0 ? (
        <div className="project-validation-errors" role="alert">
          {hostAliasIssues.map((message) => (
            <p key={message}>
              <strong>
                {hostAliasChecks.validationIssues.includes(message)
                  ? 'invalid_host_alias'
                  : 'limit_exceeded'}
              </strong>
              <span>{message}</span>
            </p>
          ))}
        </div>
      ) : null}

      <div className="network-policy-form">
        <section className="host-bindings-card" aria-labelledby="host-bindings-title">
          <div className="host-bindings-heading">
            <div>
              <p className="eyebrow">HOST BINDINGS</p>
              <h3 id="host-bindings-title">域名绑定</h3>
              <p>
                主域名与额外域名共享本 Project 的 Routes、网络策略和 Release。只支持精确 DNS
                域名；额外域名不会新增 Project 或 rnacos route Data ID。
              </p>
            </div>
            <span className="status-chip status-chip-unknown">
              {model.hostAliases.length + 1} / {routeLimits?.maxHosts ?? '—'} hosts
            </span>
          </div>
          <div className="host-binding-primary">
            <span>主域名</span>
            <strong>{project.domain}</strong>
            <small>Project identity · 不可修改</small>
          </div>
          <div className="host-alias-list" aria-label="额外域名列表">
            {model.hostAliases.length > 0 ? (
              model.hostAliases.map((alias) => (
                <div className="host-alias-chip" key={alias}>
                  <span>{alias}</span>
                  <button
                    aria-label={`移除额外域名 ${alias}`}
                    disabled={loading}
                    onClick={() => removeHostAlias(alias)}
                    type="button"
                  >
                    ×
                  </button>
                </div>
              ))
            ) : (
              <span className="host-alias-empty">尚未添加额外域名</span>
            )}
          </div>
          <div className="host-alias-form">
            <label>
              添加精确域名
              <input
                aria-label="添加额外域名"
                autoComplete="off"
                disabled={loading}
                maxLength={routeLimits?.maxHostPatternBytes ?? 255}
                placeholder="www.example.com"
                value={hostAliasInput}
                onChange={(event) => setHostAliasInput(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === 'Enter') {
                    event.preventDefault()
                    addHostAlias()
                  }
                }}
              />
            </label>
            <button
              className="button-secondary"
              disabled={loading}
              onClick={addHostAlias}
              type="button"
            >
              添加额外域名
            </button>
          </div>
        </section>

        <div className="capability-notice" role="note">
          强制 HTTPS 仅在可信 Ingress/LB 同时接收 HTTP，并清洗、设置 X-Forwarded-Proto 时生效。 当前
          access-server 直连 TLS 监听器不接收明文 HTTP。
        </div>
        <fieldset disabled={loading}>
          <legend>HTTPS 强制策略</legend>
          <label className="policy-option">
            <input
              checked={model.networkPolicy.httpsRedirect !== 'off'}
              type="checkbox"
              onChange={(event) =>
                setModel((current) => ({
                  ...current,
                  networkPolicy: {
                    ...current.networkPolicy,
                    httpsRedirect: event.target.checked ? '308' : 'off',
                  },
                }))
              }
            />
            <span>
              <strong>强制 HTTPS</strong>
              <small>Host 匹配后、Route 匹配前，将 HTTP 请求重定向到同 Host 和 URI。</small>
            </span>
          </label>
          <label className="https-redirect-status">
            重定向状态码
            <select
              disabled={model.networkPolicy.httpsRedirect === 'off' || loading}
              value={
                model.networkPolicy.httpsRedirect === 'off'
                  ? '308'
                  : model.networkPolicy.httpsRedirect
              }
              onChange={(event) =>
                setModel((current) => ({
                  ...current,
                  networkPolicy: {
                    ...current.networkPolicy,
                    httpsRedirect: event.target.value as Exclude<HttpsRedirect, 'off'>,
                  },
                }))
              }
            >
              <option value="301">301 · 永久，可能改为 GET</option>
              <option value="302">302 · 临时，可能改为 GET</option>
              <option value="307">307 · 临时，保留方法和请求体</option>
              <option value="308">308 · 永久，保留方法和请求体</option>
            </select>
            <small>301/308 可能被客户端缓存；首次灰度建议 307，稳定后可切换 308。</small>
          </label>
        </fieldset>

        <div className="form-actions">
          <span>修改已进入 Project Working Copy；请使用页面顶部的 Project 级“保存为版本”。</span>
        </div>
      </div>
    </section>
  )
}
