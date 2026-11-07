import { useState } from 'react'
import { Link } from 'react-router'

import type { WafGroup, WafGroupExemption, WafMode } from '../api/types'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useProjectContext } from './ProjectLayout'
import { useWafPolicyDraft } from './useWafPolicyDraft'
import {
  groupMembership,
  groupsInExecutionOrder,
  modeLabels,
  type WafGroupMembership,
} from './wafRuleForm'

// 项目策略页 (§4.4): per-project WAF mode override and group exemptions. Both
// edit the environment-level WAF policy draft (a singleton), scoped to this
// project's domain; exemptions always carry an auditable reason.
// An off group publishes no function whatever the project override says, and an
// off override excludes the project from every group, so neither is in effect.
function groupStatus(
  group: WafGroup,
  membership: WafGroupMembership,
  override: WafMode | undefined,
): { label: string; detail: string; active: boolean } {
  if (membership.state === 'outside') {
    return { label: '不适用', detail: '未包含该 Project', active: false }
  }
  if (membership.state === 'exempt') {
    const { exact, project, reason } = membership
    const detail = exact ? reason : `由 ${project} 豁免：${reason}（请在 WAF 页修改）`
    return { label: '已豁免', detail, active: false }
  }
  if (group.mode === 'off') {
    return { label: '未开启', detail: '规则组已关闭', active: false }
  }
  if (override === 'off') {
    return { label: '已关闭', detail: '该 Project 的模式覆盖为关闭', active: false }
  }
  return { label: '生效', detail: `模式：${modeLabels[override ?? group.mode]}`, active: true }
}

export function ProjectWafPolicyPage() {
  const { project } = useProjectContext()
  const { policy, dirty, loading, saving, errorMessage, updatePolicy, discard, save } =
    useWafPolicyDraft()
  const [exemptGroupId, setExemptGroupId] = useState('')
  const [exemptReason, setExemptReason] = useState('')

  useUnsavedChangesGuard(dirty, '当前 WAF 策略修改尚未保存，确定离开吗？')

  const domain = project.domain
  const override = policy?.projectModes[domain]

  const setOverride = (mode: WafMode | 'inherit'): void =>
    updatePolicy((current) => {
      const projectModes = { ...current.projectModes }
      if (mode === 'inherit') delete projectModes[domain]
      else projectModes[domain] = mode
      return { ...current, projectModes }
    })

  const memberships = groupsInExecutionOrder(policy?.groups ?? []).map((group) => ({
    group,
    membership: groupMembership(group, domain),
  }))
  const exemptableGroups = memberships
    .filter(({ membership }) => membership.state === 'active')
    .map(({ group }) => group)

  const setExemptions = (
    groupId: string,
    update: (current: readonly WafGroupExemption[]) => readonly WafGroupExemption[],
  ): void =>
    updatePolicy((current) => ({
      ...current,
      groups: current.groups.map((group) => {
        if (group.id !== groupId) return group
        const exempt = update(group.projects.exempt ?? [])
        const { include } = group.projects
        return { ...group, projects: exempt.length > 0 ? { include, exempt } : { include } }
      }),
    }))

  const removeExemption = (groupId: string): void =>
    setExemptions(groupId, (current) => current.filter((entry) => entry.project !== domain))

  const addExemption = (): void => {
    const reason = exemptReason.trim()
    if (!exemptGroupId || !reason) return
    setExemptions(exemptGroupId, (current) => [...current, { project: domain, reason }])
    setExemptGroupId('')
    setExemptReason('')
  }

  if (loading) {
    return <div className="route-empty-state page-loading-state">正在加载 Project WAF 策略…</div>
  }

  return (
    <section className="project-subpage" aria-labelledby="project-waf-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT WAF POLICY</p>
          <h2 id="project-waf-title">WAF 策略</h2>
          <p>
            该 Project 的模式覆盖与规则组豁免；修改保存在同一个环境草稿中，
            <Link to="/waf">在 WAF 页统一发布</Link>。
          </p>
        </div>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>WAF 策略操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      {policy === null ? (
        <div className="route-empty-state">WAF 策略草稿不可用。</div>
      ) : (
        <>
          <section className="project-workspace-bar" aria-label="Project WAF 工作区">
            <div>
              <strong>{domain}</strong>
              <span>{override ? `模式覆盖：${modeLabels[override]}` : '跟随各规则组模式'}</span>
              <span className={`status-chip status-chip-${dirty ? 'pending' : 'ready'}`}>
                {dirty ? '有未保存修改' : '已同步'}
              </span>
            </div>
            <div className="header-actions">
              <button
                className="button-secondary"
                disabled={!dirty || saving}
                onClick={() => discard()}
                type="button"
              >
                放弃修改
              </button>
              <button
                className="button-primary"
                disabled={!dirty || saving}
                onClick={() => void save()}
                type="button"
              >
                {saving ? '保存中…' : '保存到环境草稿'}
              </button>
            </div>
          </section>

          <section className="settings-card" aria-labelledby="project-waf-mode-title">
            <h3 id="project-waf-mode-title">模式覆盖</h3>
            <p className="settings-note">
              默认跟随所在规则组的模式；选择后对该 Project 所在的全部规则组生效。
            </p>
            <div className="policy-option" role="radiogroup" aria-label="模式覆盖">
              <label>
                <input
                  checked={override === undefined}
                  name="project-waf-mode"
                  onChange={() => setOverride('inherit')}
                  type="radio"
                  value="inherit"
                />
                跟随规则组
              </label>
              {(['off', 'log', 'block'] as const).map((mode) => (
                <label key={mode}>
                  <input
                    checked={override === mode}
                    name="project-waf-mode"
                    onChange={() => setOverride(mode)}
                    type="radio"
                    value={mode}
                  />
                  {modeLabels[mode]}
                </label>
              ))}
            </div>
          </section>

          <section className="settings-card" aria-labelledby="project-waf-groups-title">
            <h3 id="project-waf-groups-title">规则组</h3>
            <p className="settings-note">
              豁免让该 Project 跳过整个规则组；原因必填并随策略保存，可审计。
            </p>
            {memberships.length === 0 ? (
              <p className="settings-note">环境中还没有规则组。</p>
            ) : (
              <div className="release-resources waf-project-groups">
                {memberships.map(({ group, membership }) => {
                  const status = groupStatus(group, membership, override)
                  return (
                    <div key={group.id}>
                      <span>
                        <strong>{group.name}</strong>
                        <small title={status.detail}>{status.detail}</small>
                      </span>
                      {membership.state === 'exempt' && membership.exact ? (
                        <button
                          className="button-link"
                          onClick={() => removeExemption(group.id)}
                          title={`移除 ${group.name} 的豁免`}
                          type="button"
                        >
                          移除豁免
                        </button>
                      ) : (
                        <span
                          className={`status-chip status-chip-${status.active ? 'ready' : 'unknown'}`}
                        >
                          {status.label}
                        </span>
                      )}
                    </div>
                  )
                })}
              </div>
            )}
            {exemptableGroups.length > 0 ? (
              <div className="waf-exemption-form">
                <select
                  aria-label="选择要豁免的规则组"
                  value={exemptGroupId}
                  onChange={(event) => setExemptGroupId(event.target.value)}
                >
                  <option value="">选择规则组…</option>
                  {exemptableGroups.map((group) => (
                    <option key={group.id} value={group.id}>
                      {group.name}
                    </option>
                  ))}
                </select>
                <input
                  aria-label="豁免原因"
                  maxLength={255}
                  placeholder="豁免原因（必填，审计可见）"
                  value={exemptReason}
                  onChange={(event) => setExemptReason(event.target.value)}
                />
                <button
                  className="button-secondary"
                  disabled={!exemptGroupId || exemptReason.trim().length === 0}
                  onClick={addExemption}
                  type="button"
                >
                  添加豁免
                </button>
              </div>
            ) : null}
          </section>
        </>
      )}
    </section>
  )
}
