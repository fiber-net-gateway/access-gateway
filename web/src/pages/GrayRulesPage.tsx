import { useEffect, useMemo, useState } from 'react'
import { Link } from 'react-router'

import {
  ApiClientError,
  createGrayRulesDraft,
  fetchGrayLive,
  fetchGrayRulesDraft,
  saveGrayRulesDraft,
  validateGrayRulesPreview,
} from '../api/client'
import type {
  GrayLiveView,
  GrayRule,
  GrayRuleEntry,
  GrayRulesDocument,
  GrayRulesDraftView,
  GrayValidationPreviewView,
} from '../api/types'
import { useUnsavedChangesGuard } from '../routes/useUnsavedChangesGuard'

const ALL_ENTRIES: readonly GrayRuleEntry[] = ['internet', 'vdi', 'desktop', 'custom']

const ENTRY_LABELS: Record<GrayRuleEntry, { name: string; description: string }> = {
  internet: { name: 'internet（公网入口）', description: '来自互联网公网入口的请求' },
  vdi: { name: 'vdi（VDI 入口）', description: '来自虚拟桌面（VDI）入口的请求' },
  desktop: { name: 'desktop（办公网入口）', description: '来自办公网桌面入口的请求' },
  custom: { name: 'custom（自定义入口）', description: '来自自定义入口网络的请求' },
}

interface EditableGrayRule {
  entry: GrayRuleEntry
  ratioText: string
  cidrsText: string
}

function toEditableRules(document: GrayRulesDocument): EditableGrayRule[] {
  return document.rules.map((rule) => ({
    entry: rule.entry,
    ratioText: String(rule.ratio),
    cidrsText: rule.cidrs.join('\n'),
  }))
}

function toDocument(rules: readonly EditableGrayRule[]): {
  document: GrayRulesDocument
  localIssues: readonly { field: string; message: string }[]
} {
  const localIssues: { field: string; message: string }[] = []
  const converted: GrayRule[] = rules.map((rule, index) => {
    const trimmedRatio = rule.ratioText.trim()
    const parsedRatio = /^(0|[1-9][0-9]*)$/u.test(trimmedRatio) ? Number(trimmedRatio) : Number.NaN
    if (!Number.isInteger(parsedRatio) || parsedRatio < 0 || parsedRatio > 10_000) {
      localIssues.push({
        field: `rules[${index}].ratio`,
        message: '万分比必须为 0 到 10000 之间的整数',
      })
    }
    const cidrs = rule.cidrsText
      .split(/\r?\n/u)
      .map((line) => line.trim())
      .filter((line) => line.length > 0)
    return {
      entry: rule.entry,
      ratio: Number.isInteger(parsedRatio) ? parsedRatio : -1,
      cidrs,
    }
  })
  return {
    document: { schemaVersion: 1, rules: converted },
    localIssues,
  }
}

function formatPercentage(ratioText: string): string {
  const trimmed = ratioText.trim()
  if (!/^(0|[1-9][0-9]*)$/u.test(trimmed)) return '无效'
  const value = Number(trimmed)
  if (value < 0 || value > 10_000) return '超范围'
  return `${(value / 100).toFixed(2)}%`
}

export function GrayRulesPage() {
  const [draft, setDraft] = useState<GrayRulesDraftView | null>(null)
  const [rules, setRules] = useState<EditableGrayRule[]>([])
  const [live, setLive] = useState<GrayLiveView | null>(null)
  const [liveError, setLiveError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)
  const [saveError, setSaveError] = useState<string | null>(null)
  const [saveMessage, setSaveMessage] = useState<string | null>(null)
  const [validating, setValidating] = useState(false)
  const [validationError, setValidationError] = useState<string | null>(null)
  const [preview, setPreview] = useState<GrayValidationPreviewView | null>(null)

  useEffect(() => {
    const controller = new AbortController()
    void (async () => {
      try {
        const loaded = await fetchGrayRulesDraft(controller.signal).catch(
          async (error: unknown) => {
            if (error instanceof ApiClientError && error.status === 404) {
              return createGrayRulesDraft()
            }
            throw error
          },
        )
        if (controller.signal.aborted) return
        setDraft(loaded)
        setRules(toEditableRules(loaded.document))
        setLoadError(null)
      } catch (error) {
        if (!controller.signal.aborted) {
          setLoadError(error instanceof Error ? error.message : '加载灰度规则草稿失败')
        }
      } finally {
        if (!controller.signal.aborted) setLoading(false)
      }
    })()

    void fetchGrayLive(controller.signal)
      .then((view) => {
        if (!controller.signal.aborted) {
          setLive(view)
          setLiveError(null)
        }
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setLive(null)
          setLiveError(error instanceof Error ? error.message : '无法读取 rnacos 当前灰度配置')
        }
      })

    return () => controller.abort()
  }, [])

  const { document: currentDocument, localIssues } = useMemo(() => toDocument(rules), [rules])

  const dirty = useMemo(() => {
    if (!draft) return false
    return JSON.stringify(draft.document) !== JSON.stringify(currentDocument)
  }, [draft, currentDocument])

  useUnsavedChangesGuard(dirty)

  const usedEntries = useMemo(() => new Set(rules.map((rule) => rule.entry)), [rules])
  const nextAvailableEntry = ALL_ENTRIES.find((entry) => !usedEntries.has(entry)) ?? null

  const updateRule = (index: number, patch: Partial<EditableGrayRule>) => {
    setRules((current) =>
      current.map((item, itemIndex) => (itemIndex === index ? { ...item, ...patch } : item)),
    )
    setSaveMessage(null)
    setSaveError(null)
    setPreview(null)
  }

  const addRule = () => {
    if (!nextAvailableEntry) return
    setRules((current) => [
      ...current,
      { entry: nextAvailableEntry, ratioText: '0', cidrsText: '' },
    ])
    setSaveMessage(null)
    setSaveError(null)
    setPreview(null)
  }

  const removeRule = (index: number) => {
    setRules((current) => current.filter((_, itemIndex) => itemIndex !== index))
    setSaveMessage(null)
    setSaveError(null)
    setPreview(null)
  }

  const resetChanges = () => {
    if (!draft) return
    setRules(toEditableRules(draft.document))
    setSaveError(null)
    setSaveMessage(null)
    setPreview(null)
    setValidationError(null)
  }

  const runValidation = async () => {
    setValidationError(null)
    setPreview(null)
    if (localIssues.length > 0) {
      setValidationError(localIssues.map((issue) => `${issue.field}: ${issue.message}`).join('；'))
      return
    }
    setValidating(true)
    try {
      const result = await validateGrayRulesPreview(currentDocument)
      setPreview(result)
    } catch (error) {
      setValidationError(error instanceof Error ? error.message : '校验灰度规则失败')
    } finally {
      setValidating(false)
    }
  }

  const saveDraft = async () => {
    if (!draft) return
    setSaveError(null)
    setSaveMessage(null)
    if (localIssues.length > 0) {
      setSaveError(localIssues.map((issue) => `${issue.field}: ${issue.message}`).join('；'))
      return
    }
    setSaving(true)
    try {
      const saved = await saveGrayRulesDraft(currentDocument, draft.lockVersion)
      setDraft(saved)
      setRules(toEditableRules(saved.document))
      setSaveMessage(`草稿已保存（锁版本 ${saved.lockVersion}）；尚未发布到 rnacos。`)
    } catch (error) {
      if (error instanceof ApiClientError && error.fields.length > 0) {
        setSaveError(error.fields.map((field) => `${field.path}: ${field.message}`).join('；'))
      } else {
        setSaveError(error instanceof Error ? error.message : '保存草稿失败')
      }
    } finally {
      setSaving(false)
    }
  }

  if (loading) {
    return (
      <section className="project-subpage" aria-label="灰度规则">
        <div className="route-empty-state">正在加载灰度规则草稿…</div>
      </section>
    )
  }

  if (loadError) {
    return (
      <section className="project-subpage" aria-label="灰度规则">
        <div className="error-banner" role="alert">
          <strong>加载灰度规则失败</strong>
          <span>{loadError}</span>
        </div>
      </section>
    )
  }

  return (
    <section className="project-subpage" aria-label="灰度规则">
      <div className="subpage-meta subpage-meta-actions">
        <p className="subpage-lead">
          配置全环境灰度匹配规则（<code>ploto.unified-access.gray-match</code> ·{' '}
          <code>DEFAULT_GROUP</code>）。草稿保存不会影响数据面；需创建 Release 并发布到 rnacos，
          再通过激活证据确认各实例已生效。
        </p>
        <div className="header-actions">
          <button
            className="button-secondary"
            disabled={validating}
            onClick={() => void runValidation()}
            type="button"
          >
            {validating ? '校验中…' : '校验并预览 Wire JSON'}
          </button>
          {dirty ? (
            <button
              className="button-secondary"
              disabled={saving}
              onClick={resetChanges}
              type="button"
            >
              还原修改
            </button>
          ) : null}
          <button
            className="button-primary"
            disabled={saving || !draft || localIssues.length > 0}
            onClick={() => void saveDraft()}
            type="button"
          >
            {saving ? '保存中…' : '保存草稿'}
          </button>
          <Link className="button-secondary" to="/gray/releases">
            前往发布与回滚
          </Link>
        </div>
      </div>

      <section className="settings-card" aria-label="生命周期状态">
        <div className="gray-lifecycle-strip">
          <div>
            <span className="status-chip status-chip-neutral">草稿状态</span>
            <strong>
              {dirty ? '草稿有未保存修改' : `草稿已保存 · 版本 ${draft?.lockVersion ?? '0'}`}
            </strong>
            <small>{draft ? `SHA-256: ${draft.documentSha256.slice(0, 12)}…` : '尚未初始化'}</small>
          </div>
          <div>
            <span className={`status-chip status-chip-${live?.exists ? 'ready' : 'unknown'}`}>
              rnacos 发布状态
            </span>
            <strong>
              {live
                ? live.exists
                  ? `rnacos 已存在（${live.document?.rules.length ?? 0} 条入口配置）`
                  : 'rnacos 尚未发布灰度配置'
                : 'rnacos 状态不可用'}
            </strong>
            <small>
              {live?.sha256
                ? `SHA-256: ${live.sha256.slice(0, 12)}…`
                : (liveError ?? '未读取到远端摘要')}
            </small>
          </div>
          <div>
            <span className="status-chip status-chip-unknown">实例激活状态</span>
            <strong>以 Release 激活证据为准</strong>
            <small>rnacos 读回不等于实例已激活；请在“发布与回滚”查看每实例证据</small>
          </div>
        </div>
      </section>

      {saveMessage ? (
        <div className="release-polling-state" role="status">
          {saveMessage}
        </div>
      ) : null}

      {saveError ? (
        <div className="error-banner" role="alert">
          <strong>保存草稿失败</strong>
          <span>{saveError}</span>
        </div>
      ) : null}

      {validationError ? (
        <div className="error-banner" role="alert">
          <strong>校验未通过</strong>
          <span>{validationError}</span>
        </div>
      ) : null}

      <section className="settings-card" aria-labelledby="gray-rules-editor-title">
        <header className="gray-section-header">
          <div>
            <h3 id="gray-rules-editor-title">入口灰度规则</h3>
            <p className="settings-note">
              按入口网络（<code>internet</code> / <code>vdi</code> / <code>desktop</code> /{' '}
              <code>custom</code>）独立配置灰度放量万分比（<code>0</code>–<code>10000</code>
              ，例如 <code>500</code> 表示 <code>5.00%</code>）与 IPv4/IPv6 CIDR 命中列表。
            </p>
          </div>
          <button
            className="button-secondary"
            disabled={nextAvailableEntry === null}
            onClick={addRule}
            type="button"
          >
            + 添加入口规则
          </button>
        </header>

        {rules.length === 0 ? (
          <div className="route-empty-state">
            <h4>尚未配置灰度规则</h4>
            <p>
              当前草稿为空。若发布空草稿，控制台将向 rnacos 写入 <code>{'{}'}</code>{' '}
              以显式清空全环境灰度规则。
            </p>
          </div>
        ) : (
          <div className="gray-rule-list">
            {rules.map((rule, index) => {
              const entryOptions = ALL_ENTRIES.filter(
                (candidate) => candidate === rule.entry || !usedEntries.has(candidate),
              )
              const cidrCount = rule.cidrsText
                .split(/\r?\n/u)
                .map((line) => line.trim())
                .filter((line) => line.length > 0).length
              const numericRatio = /^(0|[1-9][0-9]*)$/u.test(rule.ratioText.trim())
                ? Math.min(10_000, Math.max(0, Number(rule.ratioText.trim())))
                : 0
              return (
                <article
                  className="gray-rule-card"
                  key={`${rule.entry}-${index}`}
                  aria-label={`灰度规则 ${index + 1}（${rule.entry}）`}
                >
                  <div className="gray-rule-card-header">
                    <div>
                      <strong>{ENTRY_LABELS[rule.entry].name}</strong>
                      <small>{ENTRY_LABELS[rule.entry].description}</small>
                    </div>
                    <button
                      className="button-secondary"
                      onClick={() => removeRule(index)}
                      type="button"
                    >
                      移除 {rule.entry}
                    </button>
                  </div>

                  <div className="gray-rule-fields">
                    <label className="gray-field">
                      <span>入口网络</span>
                      <select
                        aria-label={`规则 ${index + 1} 入口网络`}
                        value={rule.entry}
                        onChange={(event) =>
                          updateRule(index, { entry: event.target.value as GrayRuleEntry })
                        }
                      >
                        {entryOptions.map((option) => (
                          <option key={option} value={option}>
                            {ENTRY_LABELS[option].name}
                          </option>
                        ))}
                      </select>
                    </label>

                    <div className="gray-field">
                      <label htmlFor={`gray-ratio-${index}`}>
                        灰度万分比（0–10000，当前 {formatPercentage(rule.ratioText)}）
                      </label>
                      <div className="gray-ratio-controls">
                        <input
                          id={`gray-ratio-${index}`}
                          aria-label={`规则 ${index + 1} 灰度万分比`}
                          inputMode="numeric"
                          max={10000}
                          min={0}
                          step={1}
                          type="number"
                          value={rule.ratioText}
                          onChange={(event) => updateRule(index, { ratioText: event.target.value })}
                        />
                        <input
                          aria-label={`规则 ${index + 1} 灰度万分比滑块`}
                          max={10000}
                          min={0}
                          step={10}
                          type="range"
                          value={numericRatio}
                          onChange={(event) => updateRule(index, { ratioText: event.target.value })}
                        />
                        <span className="status-chip status-chip-neutral">
                          {formatPercentage(rule.ratioText)}
                        </span>
                      </div>
                    </div>

                    <label className="gray-field gray-field-wide">
                      <span>白名单 CIDR（每行一条 IPv4/IPv6 CIDR，当前 {cidrCount} 条）</span>
                      <textarea
                        aria-label={`规则 ${index + 1} CIDR 列表`}
                        placeholder={'10.0.0.0/8\n2001:db8::/32'}
                        rows={4}
                        value={rule.cidrsText}
                        onChange={(event) => updateRule(index, { cidrsText: event.target.value })}
                      />
                    </label>
                  </div>
                </article>
              )
            })}
          </div>
        )}
      </section>

      {preview ? (
        <section className="settings-card" aria-labelledby="gray-validation-preview-title">
          <h3 id="gray-validation-preview-title">校验与 Wire 预览</h3>
          <div className="waf-compile-summary">
            <p>
              <span
                className={`status-chip status-chip-${
                  preview.valid && preview.nativeValid !== false ? 'ready' : 'pending'
                }`}
              >
                {preview.valid && preview.nativeValid !== false ? '校验通过' : '校验未通过'}
              </span>{' '}
              <span
                className={`status-chip status-chip-${
                  preview.nativeValid === true
                    ? 'ready'
                    : preview.nativeValid === false
                      ? 'pending'
                      : 'unknown'
                }`}
              >
                Native Validator：
                {preview.nativeValid === true
                  ? '已通过'
                  : preview.nativeValid === false
                    ? '拒绝'
                    : '不可用（未执行原生校验）'}
              </span>{' '}
              有效规则数：{preview.ruleCount}
              {preview.validator ? ` · 校验器版本 ${preview.validator.revision.slice(0, 12)}` : ''}
            </p>

            {preview.issues.length > 0 ? (
              <ul className="gray-error-list" role="alert">
                {preview.issues.map((issue, idx) => (
                  <li key={`${issue.field}-${idx}`}>
                    <code>{issue.field}</code>（{issue.code}）：{issue.message}
                  </li>
                ))}
              </ul>
            ) : null}

            {preview.nativeErrors.length > 0 ? (
              <ul className="gray-error-list" role="alert">
                {preview.nativeErrors.map((error, idx) => (
                  <li key={`${error.code}-${idx}`}>
                    <code>{error.field ?? 'payload'}</code>（{error.code}）：{error.message}
                  </li>
                ))}
              </ul>
            ) : null}

            {preview.wirePreview !== null ? (
              <details className="wire-preview" open>
                <summary>
                  目标 Data ID：{preview.dataId}（{preview.group}） · SHA-256：
                  {preview.wireSha256?.slice(0, 16)}…
                </summary>
                <pre>{preview.wirePreview}</pre>
              </details>
            ) : null}
          </div>
        </section>
      ) : null}
    </section>
  )
}
