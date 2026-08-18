import { useEffect, useMemo, useState, type FormEvent } from 'react'

import { fetchCurrentConfigurationVersion, saveConfigurationVersion } from '../api/client'
import type { HttpsRedirect, ProjectRoutesModel } from '../api/types'
import { initialRouteModel, normalizeExactHost, validateHostAliases } from '../routes/model'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useProjectContext } from './ProjectLayout'

export function ProjectHostPolicyPage() {
  const { project, refreshProject, systemStatus } = useProjectContext()
  const [model, setModel] = useState<ProjectRoutesModel>(initialRouteModel)
  const [baseVersionId, setBaseVersionId] = useState<string | null>(null)
  const [baseVersionNumber, setBaseVersionNumber] = useState<number | null>(null)
  const [lockVersion, setLockVersion] = useState('0')
  const [savedHostAliases, setSavedHostAliases] = useState<readonly string[]>([])
  const [savedHttpsRedirect, setSavedHttpsRedirect] = useState<HttpsRedirect>('off')
  const [hostAliasInput, setHostAliasInput] = useState('')
  const [changeSummary, setChangeSummary] = useState('更新域名与 HTTPS 策略')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
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
  const dirty =
    !loading &&
    (JSON.stringify(model.hostAliases) !== JSON.stringify(savedHostAliases) ||
      model.networkPolicy.httpsRedirect !== savedHttpsRedirect)
  useUnsavedChangesGuard(dirty)

  useEffect(() => {
    const controller = new AbortController()
    setLoading(true)
    void fetchCurrentConfigurationVersion(project.id, controller.signal)
      .then((current) => {
        const next = current?.version.model ?? initialRouteModel()
        setModel(next)
        setSavedHostAliases(next.hostAliases)
        setSavedHttpsRedirect(next.networkPolicy.httpsRedirect)
        setBaseVersionId(current?.version.id ?? null)
        setBaseVersionNumber(current?.version.number ?? null)
        setLockVersion(current?.lockVersion ?? project.draft?.lockVersion ?? '0')
        setErrorMessage(null)
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : '加载 Host Policy 失败')
        }
      })
      .finally(() => {
        if (!controller.signal.aborted) setLoading(false)
      })
    return () => controller.abort()
  }, [project.draft?.lockVersion, project.id])

  const submit = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setSaving(true)
    setErrorMessage(null)
    try {
      if (hostAliasIssues.length > 0) throw new Error(hostAliasIssues[0])
      const saved = await saveConfigurationVersion(
        project.id,
        lockVersion,
        baseVersionId,
        changeSummary,
        model,
      )
      setModel(saved.version.model)
      setSavedHostAliases(saved.version.model.hostAliases)
      setSavedHttpsRedirect(saved.version.model.networkPolicy.httpsRedirect)
      setBaseVersionId(saved.version.id)
      setBaseVersionNumber(saved.version.number)
      setLockVersion(saved.lockVersion)
      await refreshProject()
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '保存 Host Policy 失败')
    } finally {
      setSaving(false)
    }
  }

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
        <span className={`status-chip status-chip-${dirty ? 'pending' : 'ready'}`}>
          {dirty
            ? '有未保存修改'
            : baseVersionNumber
              ? `已保存于 V${baseVersionNumber}`
              : '尚无版本'}
        </span>
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

      <form className="network-policy-form" onSubmit={(event) => void submit(event)}>
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
                    disabled={loading || saving}
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
                disabled={loading || saving}
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
              disabled={loading || saving}
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
        <fieldset disabled={loading || saving}>
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
              disabled={model.networkPolicy.httpsRedirect === 'off' || loading || saving}
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

        <label className="network-change-summary">
          版本说明
          <input
            maxLength={200}
            required
            value={changeSummary}
            onChange={(event) => setChangeSummary(event.target.value)}
          />
        </label>
        <div className="form-actions">
          <span>域名和 HTTPS 策略会随配置版本发布；保存本身不代表实例已经激活。</span>
          <button
            className="button-primary"
            disabled={!dirty || saving || loading || hostAliasIssues.length > 0}
            type="submit"
          >
            {saving
              ? '保存中…'
              : `保存为${baseVersionNumber ? ` V${baseVersionNumber + 1}` : ' V1'}`}
          </button>
        </div>
      </form>
    </section>
  )
}
