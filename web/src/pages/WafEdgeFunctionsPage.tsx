import { useCallback, useEffect, useState } from 'react'

import { fetchWafEdgeFunctions } from '../api/client'
import type { WafEdgeFunctionsView } from '../api/types'

// 边缘函数页 v1 只读 (§4.4): exactly what the data plane currently sees in
// rnacos — the function index plus script bytes. Authoring handwritten
// functions opens in P3.
export function WafEdgeFunctionsPage() {
  const [view, setView] = useState<WafEdgeFunctionsView | null>(null)
  const [loading, setLoading] = useState(true)
  const [refreshing, setRefreshing] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const load = useCallback(async (signal?: AbortSignal, background = false): Promise<void> => {
    if (background) setRefreshing(true)
    else setLoading(true)
    try {
      setView(await fetchWafEdgeFunctions(signal))
      setErrorMessage(null)
    } finally {
      setLoading(false)
      setRefreshing(false)
    }
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    void load(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载边缘函数快照失败')
      }
    })
    return () => controller.abort()
  }, [load])

  return (
    <section className="project-subpage" aria-labelledby="waf-edge-functions-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">EDGE FUNCTIONS · LIVE</p>
          <h2 id="waf-edge-functions-title">边缘函数（只读）</h2>
          <p>
            直接读取 rnacos 中数据面当前使用的函数索引与脚本字节；v1
            仅展示，手写函数将在后续版本开放。
          </p>
        </div>
        <button
          className="button-secondary"
          disabled={loading || refreshing}
          onClick={() =>
            void load(undefined, true).catch((error: unknown) => {
              setErrorMessage(error instanceof Error ? error.message : '刷新边缘函数快照失败')
            })
          }
          type="button"
        >
          {refreshing ? '刷新中…' : '刷新快照'}
        </button>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>快照加载失败</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      {loading ? (
        <div className="route-empty-state">正在读取 rnacos 中的边缘函数…</div>
      ) : view === null ? null : (
        <>
          <section className="settings-card" aria-labelledby="waf-edge-index-title">
            <h3 id="waf-edge-index-title">函数索引</h3>
            {!view.index.exists ? (
              <p className="settings-note">rnacos 中尚无函数索引（未发布过边缘函数）。</p>
            ) : (
              <>
                <div className="project-status-row">
                  <span>索引版本：{view.index.version ?? '未知'}</span>
                  <span>函数数量：{view.index.functions.length}</span>
                  <span>
                    sha256：
                    {view.index.sha256 ? `${view.index.sha256.slice(0, 16)}…` : '未知'}
                  </span>
                </div>
                {view.index.parseError ? (
                  <div className="error-banner" role="alert">
                    <strong>索引解析失败</strong>
                    <span>{view.index.parseError}</span>
                  </div>
                ) : null}
                {view.index.functions.length > 0 ? (
                  <div className="release-resources">
                    {view.index.functions.map((entry) => (
                      <div key={entry.name}>
                        <span>
                          <strong>{entry.name}</strong>
                          <small>
                            {entry.sha256 ? `脚本 ${entry.sha256.slice(0, 16)}…` : '无脚本摘要'} ·{' '}
                            {entry.mounts
                              .map(
                                (mount) =>
                                  `挂载 include=${mount.projects.include.join(',') || '*'} exclude=${
                                    mount.projects.exclude.join(',') || '-'
                                  } 优先级 ${mount.priority ?? '未知'}`,
                              )
                              .join('；')}
                          </small>
                        </span>
                      </div>
                    ))}
                  </div>
                ) : (
                  <p className="settings-note">索引为空：数据面未挂载任何边缘函数。</p>
                )}
              </>
            )}
          </section>

          {view.scripts.map((script) => (
            <section
              aria-labelledby={`waf-script-${script.name}`}
              className="settings-card"
              key={script.name}
            >
              <h3 id={`waf-script-${script.name}`}>脚本：{script.name}</h3>
              {script.exists && script.content !== null ? (
                <>
                  <p className="settings-note">
                    {script.content.split('\n').length} 行 · sha256{' '}
                    {script.sha256 ? `${script.sha256.slice(0, 16)}…` : '未知'}
                  </p>
                  <details className="wire-preview">
                    <summary>查看脚本内容</summary>
                    <pre>{script.content}</pre>
                  </details>
                </>
              ) : (
                <p className="settings-note">索引引用了该函数，但 rnacos 中没有对应脚本字节。</p>
              )}
            </section>
          ))}
        </>
      )}
    </section>
  )
}
