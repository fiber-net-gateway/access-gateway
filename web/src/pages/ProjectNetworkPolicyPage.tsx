import { useEffect, useMemo, useState, type FormEvent } from 'react'

import { fetchCurrentConfigurationVersion, saveConfigurationVersion } from '../api/client'
import type { ProjectNetworkPolicy, ProjectRoutesModel } from '../api/types'
import { initialRouteModel } from '../routes/model'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
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

export function ProjectNetworkPolicyPage() {
  const { project, refreshProject } = useProjectContext()
  const [model, setModel] = useState<ProjectRoutesModel>(initialRouteModel)
  const [baseVersionId, setBaseVersionId] = useState<string | null>(null)
  const [baseVersionNumber, setBaseVersionNumber] = useState<number | null>(null)
  const [lockVersion, setLockVersion] = useState('0')
  const [savedPolicy, setSavedPolicy] = useState<ProjectNetworkPolicy>(
    initialRouteModel().networkPolicy,
  )
  const [allowedCidrs, setAllowedCidrs] = useState('')
  const [deniedCidrs, setDeniedCidrs] = useState('')
  const [changeSummary, setChangeSummary] = useState('更新网络策略')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const policy = useMemo<ProjectNetworkPolicy>(
    () => ({
      source: model.networkPolicy.source,
      allowedCidrs: parseLines(allowedCidrs),
      deniedCidrs: parseLines(deniedCidrs),
    }),
    [allowedCidrs, deniedCidrs, model.networkPolicy.source],
  )
  const dirty = !loading && JSON.stringify(policy) !== JSON.stringify(savedPolicy)
  useUnsavedChangesGuard(dirty)

  useEffect(() => {
    const controller = new AbortController()
    setLoading(true)
    void fetchCurrentConfigurationVersion(project.id, controller.signal)
      .then((current) => {
        const next = current?.version.model ?? initialRouteModel()
        setModel(next)
        setSavedPolicy(next.networkPolicy)
        setAllowedCidrs(lines(next.networkPolicy.allowedCidrs))
        setDeniedCidrs(lines(next.networkPolicy.deniedCidrs))
        setBaseVersionId(current?.version.id ?? null)
        setBaseVersionNumber(current?.version.number ?? null)
        setLockVersion(current?.lockVersion ?? project.draft?.lockVersion ?? '0')
        setErrorMessage(null)
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : '加载网络策略失败')
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
      const submitted = { ...model, networkPolicy: policy }
      const saved = await saveConfigurationVersion(
        project.id,
        lockVersion,
        baseVersionId,
        changeSummary,
        submitted,
      )
      setModel(saved.version.model)
      setSavedPolicy(saved.version.model.networkPolicy)
      setBaseVersionId(saved.version.id)
      setBaseVersionNumber(saved.version.number)
      setLockVersion(saved.lockVersion)
      await refreshProject()
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '保存网络策略失败')
    } finally {
      setSaving(false)
    }
  }

  return (
    <section className="project-subpage" aria-labelledby="network-policy-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT / NETWORK POLICY</p>
          <h2 id="network-policy-title">Network Policy</h2>
          <p>
            网络策略与 Routes 一起保存为不可变配置版本；只有 Release
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

      <form className="network-policy-form" onSubmit={(event) => void submit(event)}>
        <div className="capability-notice" role="note">
          兼容约束：access-server 当前读取 X-Real-Ip，头缺失或不可解析时会跳过 CIDR
          检查。生产入口必须清洗并规范设置该头。
        </div>
        <fieldset disabled={loading || saving}>
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
              disabled={model.networkPolicy.source !== 'project' || loading || saving}
              placeholder={'10.0.0.0/8\n2001:db8::/32'}
              rows={9}
              spellCheck={false}
              value={allowedCidrs}
              onChange={(event) => setAllowedCidrs(event.target.value)}
            />
            <small>非空时，请求源地址必须匹配至少一项。</small>
          </label>
          <label>
            拒绝 CIDR（每行一项）
            <textarea
              disabled={model.networkPolicy.source !== 'project' || loading || saving}
              placeholder={'10.1.0.0/16\n2001:db8:dead::/48'}
              rows={9}
              spellCheck={false}
              value={deniedCidrs}
              onChange={(event) => setDeniedCidrs(event.target.value)}
            />
            <small>拒绝规则优先；发布时编译为 native `!CIDR` 形式。</small>
          </label>
        </div>

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
          <span>空允许/拒绝列表表示公开访问；服务端会拒绝无效或重复 CIDR。</span>
          <button className="button-primary" disabled={!dirty || saving || loading} type="submit">
            {saving
              ? '保存中…'
              : `保存为${baseVersionNumber ? ` V${baseVersionNumber + 1}` : ' V1'}`}
          </button>
        </div>
      </form>
    </section>
  )
}
