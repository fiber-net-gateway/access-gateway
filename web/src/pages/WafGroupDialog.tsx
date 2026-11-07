import { useEffect, useRef, useState, type FormEvent } from 'react'

import type { WafGroup, WafMode } from '../api/types'
import { keepFocusInside } from './WafRuleDialog'
import {
  formToGroup,
  groupFormProblems,
  groupToForm,
  modeLabels,
  newWafGroupForm,
  type WafGroupForm,
} from './wafRuleForm'

const fieldLabels: Record<string, string> = {
  id: '组 ID',
  name: '组名称',
  priority: '优先级',
  include: '生效 Project',
}

interface WafGroupDialogProps {
  /** Null while adding a group. */
  group: WafGroup | null
  existingIds: readonly string[]
  ruleCount: number
  onApply(group: WafGroup): void
  onRemove(): void
  onClose(): void
  onDirtyChange(dirty: boolean): void
}

export function WafGroupDialog({
  group,
  existingIds,
  ruleCount,
  onApply,
  onRemove,
  onClose,
  onDirtyChange,
}: WafGroupDialogProps) {
  const [form, setForm] = useState(() => (group ? groupToForm(group) : newWafGroupForm()))
  const [initialFormKey] = useState(() => JSON.stringify(form))
  const [attempted, setAttempted] = useState(false)
  const [exemptProject, setExemptProject] = useState('')
  const [exemptReason, setExemptReason] = useState('')
  const [opener] = useState(() => document.activeElement)
  const nameRef = useRef<HTMLInputElement | null>(null)

  const dirty = JSON.stringify(form) !== initialFormKey
  const problems = groupFormProblems(
    form,
    existingIds.filter((id) => id !== group?.id),
  )
  const shownProblems = attempted ? problems : []

  useEffect(() => {
    onDirtyChange(dirty)
  }, [dirty, onDirtyChange])

  useEffect(() => {
    nameRef.current?.focus()
    return () => {
      if (opener instanceof HTMLElement && opener.isConnected) opener.focus()
    }
  }, [opener])

  const update = (patch: Partial<WafGroupForm>): void =>
    setForm((current) => ({ ...current, ...patch }))

  const invalid = (field: string): true | undefined =>
    shownProblems.some((problem) => problem.field === field) ? true : undefined

  const requestClose = (): void => {
    if (!dirty || window.confirm('放弃这个规则组尚未应用的修改吗？')) onClose()
  }

  const requestRemove = (): void => {
    if (ruleCount === 0 || window.confirm(`删除该规则组及其 ${ruleCount} 条规则吗？`)) onRemove()
  }

  const addExemption = (): void => {
    const project = exemptProject.trim()
    const reason = exemptReason.trim()
    if (!project || !reason) return
    update({
      exempt: [...form.exempt.filter((entry) => entry.project !== project), { project, reason }],
    })
    setExemptProject('')
    setExemptReason('')
  }

  const submit = (event: FormEvent): void => {
    event.preventDefault()
    setAttempted(true)
    if (problems.length === 0) onApply(formToGroup(form))
  }

  return (
    <div className="dialog-backdrop" role="presentation">
      <form
        aria-labelledby="waf-group-dialog-title"
        aria-modal="true"
        className="dialog-card dialog-card-wide waf-rule-dialog"
        noValidate
        onKeyDown={(event) => {
          if (event.key === 'Escape') requestClose()
          if (event.key === 'Tab') keepFocusInside(event)
        }}
        onSubmit={submit}
        role="dialog"
      >
        <header className="waf-rule-dialog-header">
          <h2 id="waf-group-dialog-title">{group ? '编辑规则组' : '新建规则组'}</h2>
          <small>
            每个规则组发布为一个边缘函数 <code>waf-{form.id.trim() || '<组 ID>'}</code>
            ；应用后写入页面草稿，点击“保存策略草稿”才会保存
          </small>
        </header>

        <div className="waf-rule-dialog-body">
          <div className="waf-group-fields">
            <label>
              组名称
              <input
                aria-invalid={invalid('name')}
                maxLength={255}
                ref={nameRef}
                value={form.name}
                onChange={(event) => update({ name: event.target.value })}
              />
            </label>
            <label>
              组 ID
              <input
                aria-invalid={invalid('id')}
                disabled={group !== null}
                maxLength={60}
                placeholder="sqli"
                title="进入函数名、指标和日志，创建后不可修改"
                value={form.id}
                onChange={(event) => update({ id: event.target.value })}
              />
            </label>
            <label>
              模式
              <select
                value={form.mode}
                onChange={(event) => update({ mode: event.target.value as WafMode })}
              >
                {(['off', 'log', 'block'] as const).map((mode) => (
                  <option key={mode} value={mode}>
                    {modeLabels[mode]}
                  </option>
                ))}
              </select>
            </label>
            <label>
              优先级
              <input
                aria-invalid={invalid('priority')}
                inputMode="numeric"
                title="数字越小越先执行；也决定与手写边缘函数的先后"
                value={form.priority}
                onChange={(event) => update({ priority: event.target.value })}
              />
            </label>
          </div>

          <label>
            生效 Project（逗号分隔，支持 * 通配）
            <input
              aria-invalid={invalid('include')}
              placeholder="*, loan-*, api.example.com"
              value={form.include}
              onChange={(event) => update({ include: event.target.value })}
            />
          </label>

          <div className="waf-exemptions">
            <span className="waf-rule-field-label">豁免 Project（{form.exempt.length}）</span>
            {form.exempt.length > 0 ? (
              <ul>
                {form.exempt.map((entry) => (
                  <li key={entry.project}>
                    <span>
                      <strong>{entry.project}</strong>
                      <small>{entry.reason}</small>
                    </span>
                    <button
                      className="button-link"
                      onClick={() =>
                        update({
                          exempt: form.exempt.filter(
                            (existing) => existing.project !== entry.project,
                          ),
                        })
                      }
                      title={`移除豁免 ${entry.project}`}
                      type="button"
                    >
                      移除豁免
                    </button>
                  </li>
                ))}
              </ul>
            ) : null}
            <div className="waf-exemption-form">
              <input
                aria-label="豁免 Project"
                placeholder="Project 名称或通配"
                value={exemptProject}
                onChange={(event) => setExemptProject(event.target.value)}
              />
              <input
                aria-label="豁免原因"
                maxLength={255}
                placeholder="豁免原因（必填，审计可见）"
                value={exemptReason}
                onChange={(event) => setExemptReason(event.target.value)}
              />
              <button
                className="button-secondary"
                disabled={!exemptProject.trim() || !exemptReason.trim()}
                onClick={addExemption}
                type="button"
              >
                添加豁免
              </button>
            </div>
          </div>
        </div>

        {shownProblems.length > 0 ? (
          <ul className="route-issues waf-rule-problems" role="alert">
            {shownProblems.map((problem) => (
              <li key={`${problem.field}-${problem.code}`}>
                <span>{fieldLabels[problem.field] ?? problem.field}</span>
                <span>{problem.code}</span>
                <span>{problem.message}</span>
              </li>
            ))}
          </ul>
        ) : null}

        <footer className="waf-rule-dialog-footer">
          {group ? (
            <button className="button-danger" onClick={requestRemove} type="button">
              删除规则组
            </button>
          ) : (
            <span />
          )}
          <div className="dialog-actions">
            <button className="button-secondary" onClick={requestClose} type="button">
              取消
            </button>
            <button className="button-primary" type="submit">
              应用到草稿
            </button>
          </div>
        </footer>
      </form>
    </div>
  )
}
