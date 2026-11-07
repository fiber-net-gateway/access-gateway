import { useState } from 'react'
import { Link } from 'react-router'

import type { WafMode } from '../api/types'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'
import { useProjectContext } from './ProjectLayout'
import { useWafPolicyDraft } from './useWafPolicyDraft'

const modeLabels: Record<WafMode, string> = {
  off: '关闭',
  log: '仅记录',
  block: '拦截',
}

// 项目策略页 (§4.4): per-project WAF mode override and exemption management.
// Both edit the environment-level WAF policy draft (a singleton), scoped to
// this project's domain; exemptions always carry an auditable reason.
export function ProjectWafPolicyPage() {
  const { project } = useProjectContext()
  const { policy, dirty, loading, saving, errorMessage, updatePolicy, discard, save } =
    useWafPolicyDraft()
  const [exclusionRuleId, setExclusionRuleId] = useState('')
  const [exclusionReason, setExclusionReason] = useState('')

  useUnsavedChangesGuard(dirty, '当前 WAF 策略修改尚未保存，确定离开吗？')

  const domain = project.domain
  const override = policy?.projectModes[domain]
  const effectiveMode: WafMode = override ?? policy?.defaultMode ?? 'off'

  const setOverride = (mode: WafMode | 'inherit'): void =>
    updatePolicy((current) => {
      const projectModes = { ...current.projectModes }
      if (mode === 'inherit') delete projectModes[domain]
      else projectModes[domain] = mode
      return { ...current, projectModes }
    })

  const applicableRules = policy?.rules ?? []
  const exemptingRules = applicableRules.filter((rule) =>
    rule.exclusions?.some((exclusion) => exclusion.project === domain),
  )

  const removeExclusion = (ruleId: string): void =>
    updatePolicy((current) => ({
      ...current,
      rules: current.rules.map((rule) =>
        rule.id === ruleId
          ? {
              ...rule,
              exclusions: (rule.exclusions ?? []).filter(
                (exclusion) => exclusion.project !== domain,
              ),
            }
          : rule,
      ),
    }))

  const addExclusion = (): void => {
    const reason = exclusionReason.trim()
    if (!exclusionRuleId || !reason) return
    updatePolicy((current) => ({
      ...current,
      rules: current.rules.map((rule) => {
        if (rule.id !== exclusionRuleId) return rule
        const exclusions = (rule.exclusions ?? []).filter(
          (exclusion) => exclusion.project !== domain,
        )
        return { ...rule, exclusions: [...exclusions, { project: domain, reason }] }
      }),
    }))
    setExclusionRuleId('')
    setExclusionReason('')
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
            该 Project 在环境级 WAF 策略中的模式与豁免；修改保存在同一个环境草稿中，
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
              <span>
                生效模式：{modeLabels[effectiveMode]}
                {override ? '（本项目覆盖）' : '（跟随环境默认）'}
              </span>
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
              环境默认模式为 {modeLabels[policy.defaultMode]}；可为该 Project 单独选择模式。
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
                继承默认（{modeLabels[policy.defaultMode]}）
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

          <section className="settings-card" aria-labelledby="project-waf-exclusions-title">
            <h3 id="project-waf-exclusions-title">规则豁免</h3>
            <p className="settings-note">
              豁免让命名规则跳过该 Project；原因必填并随策略保存，可审计。
            </p>
            {exemptingRules.length === 0 ? (
              <p className="settings-note">当前没有规则豁免该 Project。</p>
            ) : (
              <div className="release-resources">
                {exemptingRules.map((rule) => (
                  <div key={rule.id}>
                    <span>
                      <strong>{rule.name || rule.id}</strong>
                      <small>
                        {rule.exclusions?.find((exclusion) => exclusion.project === domain)?.reason}
                      </small>
                    </span>
                    <button
                      className="button-link"
                      onClick={() => removeExclusion(rule.id)}
                      title={`移除 ${rule.name || rule.id} 的豁免`}
                      type="button"
                    >
                      移除豁免
                    </button>
                  </div>
                ))}
              </div>
            )}
            {applicableRules.length > 0 ? (
              <div className="waf-exclusion-form">
                <select
                  aria-label="选择要豁免的规则"
                  value={exclusionRuleId}
                  onChange={(event) => setExclusionRuleId(event.target.value)}
                >
                  <option value="">选择规则…</option>
                  {applicableRules.map((rule) => (
                    <option key={rule.id} value={rule.id}>
                      {rule.name || rule.id}
                    </option>
                  ))}
                </select>
                <input
                  aria-label="豁免原因"
                  maxLength={255}
                  placeholder="豁免原因（必填，审计可见）"
                  value={exclusionReason}
                  onChange={(event) => setExclusionReason(event.target.value)}
                />
                <button
                  className="button-secondary"
                  disabled={!exclusionRuleId || exclusionReason.trim().length === 0}
                  onClick={addExclusion}
                  type="button"
                >
                  添加豁免
                </button>
              </div>
            ) : (
              <p className="settings-note">环境中还没有 WAF 规则，无规则可豁免。</p>
            )}
          </section>
        </>
      )}
    </section>
  )
}
