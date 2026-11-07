import { useCallback, useEffect, useMemo, useState } from 'react'

import { ApiClientError, fetchWafHits } from '../api/client'
import type { WafHitsView, WafHitsWindow } from '../api/types'
import { useWafPolicyDraft } from './useWafPolicyDraft'

const windowLabels: Record<WafHitsWindow, string> = {
  '1h': '近 1 小时',
  '24h': '近 24 小时',
  '7d': '近 7 天',
}

interface RuleHits {
  ruleId: string
  logCount: number
  blockCount: number
}

// Groups the Prometheus rows (one per event × rule) into per-rule totals.
function groupByRule(rows: readonly WafHitsView['rows'][number][]): RuleHits[] {
  const byRule = new Map<string, RuleHits>()
  for (const row of rows) {
    const entry = byRule.get(row.ruleId) ?? { ruleId: row.ruleId, logCount: 0, blockCount: 0 }
    if (row.event === 'waf.block') {
      entry.blockCount += row.count
    } else {
      entry.logCount += row.count
    }
    byRule.set(row.ruleId, entry)
  }
  return [...byRule.values()].sort(
    (a, b) =>
      b.blockCount + b.logCount - (a.blockCount + a.logCount) || a.ruleId.localeCompare(b.ruleId),
  )
}

export function WafHitsPage() {
  const { policy, loading: policyLoading } = useWafPolicyDraft()
  const [window, setWindow] = useState<WafHitsWindow>('24h')
  const [hits, setHits] = useState<WafHitsView | null>(null)
  const [loading, setLoading] = useState(true)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [unconfigured, setUnconfigured] = useState(false)

  const load = useCallback(
    (signal?: AbortSignal): void => {
      setLoading(true)
      void fetchWafHits(window, signal)
        .then((view) => {
          setHits(view)
          setErrorMessage(null)
          setUnconfigured(false)
        })
        .catch((error: unknown) => {
          if (signal?.aborted) return
          if (error instanceof ApiClientError && error.code === 'PROMETHEUS_UNCONFIGURED') {
            setUnconfigured(true)
            setErrorMessage(null)
          } else {
            setUnconfigured(false)
            setErrorMessage(error instanceof Error ? error.message : '命中统计查询失败')
          }
        })
        .finally(() => {
          if (!signal?.aborted) setLoading(false)
        })
    },
    [window],
  )

  useEffect(() => {
    const controller = new AbortController()
    load(controller.signal)
    return () => controller.abort()
  }, [load])

  const ruleNames = useMemo(() => {
    const names = new Map<string, string>()
    if (policy) {
      for (const rule of policy.rules) names.set(rule.id, rule.name)
    }
    return names
  }, [policy])
  const ruleGroupNames = useMemo(() => {
    const groups = new Map(policy?.groups.map((group) => [group.id, group.name]))
    return new Map(policy?.rules.map((rule) => [rule.id, groups.get(rule.group)]))
  }, [policy])

  const grouped = useMemo(() => (hits ? groupByRule(hits.rows) : []), [hits])
  const totalBlocks = useMemo(
    () => grouped.reduce((sum, entry) => sum + entry.blockCount, 0),
    [grouped],
  )
  const totalLogs = useMemo(
    () => grouped.reduce((sum, entry) => sum + entry.logCount, 0),
    [grouped],
  )

  return (
    <section className="project-subpage" aria-labelledby="waf-hits-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">WAF HIT STATISTICS</p>
          <h2 id="waf-hits-title">命中统计</h2>
          <p>
            数据来自数据面经 Prometheus 抓取的规则命中计数；数据面仍是事实来源，控制台只做聚合展示。
          </p>
        </div>
        <div className="header-actions">
          <button
            className="button-secondary"
            disabled={loading}
            onClick={() => load()}
            type="button"
          >
            {loading ? '查询中…' : '刷新'}
          </button>
        </div>
      </header>

      <section className="settings-card" aria-labelledby="waf-hits-window-title">
        <h3 id="waf-hits-window-title">统计窗口</h3>
        <div className="policy-option" role="radiogroup" aria-label="统计窗口">
          {(['1h', '24h', '7d'] as const).map((option) => (
            <label key={option}>
              <input
                checked={window === option}
                name="waf-hits-window"
                onChange={() => setWindow(option)}
                type="radio"
                value={option}
              />
              {windowLabels[option]}
            </label>
          ))}
        </div>
      </section>

      {unconfigured ? (
        <div className="route-empty-state">
          <h3>未配置 Prometheus</h3>
          <p>
            控制台需要 PROMETHEUS_URL 指向抓取 access-server :8001 指标的
            Prometheus，才能展示命中统计。
          </p>
        </div>
      ) : errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>命中统计查询失败</strong>
          <span>{errorMessage}</span>
        </div>
      ) : hits === null ? (
        <div className="route-empty-state page-loading-state">正在加载命中统计…</div>
      ) : (
        <section className="settings-card" aria-labelledby="waf-hits-table-title">
          <h3 id="waf-hits-table-title">
            规则命中（{windowLabels[hits.window]} · 查询于{' '}
            {new Date(hits.queriedAt).toLocaleString('zh-CN', { hour12: false })}）
          </h3>
          <p className="settings-note">
            共 {grouped.length} 条规则 · 拦截 {Math.round(totalBlocks)} 次 · 记录{' '}
            {Math.round(totalLogs)} 次
          </p>
          {grouped.length === 0 ? (
            <p className="settings-note">窗口内没有命中记录。</p>
          ) : (
            <table className="waf-hits-table">
              <thead>
                <tr>
                  <th>规则</th>
                  <th>规则组</th>
                  <th>拦截</th>
                  <th>仅记录</th>
                  <th>合计</th>
                </tr>
              </thead>
              <tbody>
                {grouped.map((entry) => {
                  const name = ruleNames.get(entry.ruleId)
                  return (
                    <tr key={entry.ruleId}>
                      <th scope="row">
                        {name ?? <em>（已不在当前策略）</em>}
                        <small>
                          {entry.ruleId}
                          {!policyLoading && name === undefined ? ' · 历史规则或外部函数事件' : ''}
                        </small>
                      </th>
                      <td>{ruleGroupNames.get(entry.ruleId) ?? '—'}</td>
                      <td>{Math.round(entry.blockCount)}</td>
                      <td>{Math.round(entry.logCount)}</td>
                      <td>{Math.round(entry.blockCount + entry.logCount)}</td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          )}
        </section>
      )}
    </section>
  )
}
