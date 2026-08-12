import { useCallback, useEffect, useMemo, useState, type FormEvent } from 'react'

import { fetchCertificates, fetchCertificateVersions, resolveTlsSni } from '../api/client'
import type { CertificateVersionView, CertificateView, TlsSniResolutionView } from '../api/types'
import { CapabilityStrip } from '../components/CapabilityStrip'
import { CertificateUploadForm } from '../components/CertificateUploadForm'
import { useConsoleContext } from '../App'

const statusLabel: Record<CertificateVersionView['status'], string> = {
  valid: '有效',
  expiring: '即将到期',
  expired: '已过期',
  superseded: '历史版本',
}

const resolutionLabel: Record<TlsSniResolutionView['resolutionStatus'], string> = {
  matched: '已匹配',
  uncovered: '未覆盖',
  conflict: '索引冲突',
}

export function CertificatesPage() {
  const { apiState, health, systemStatus, statusError } = useConsoleContext()
  const [certificates, setCertificates] = useState<readonly CertificateView[]>([])
  const [selectedId, setSelectedId] = useState<string | null>(null)
  const [versions, setVersions] = useState<readonly CertificateVersionView[]>([])
  const [previewName, setPreviewName] = useState('')
  const [resolution, setResolution] = useState<TlsSniResolutionView | null>(null)
  const [loading, setLoading] = useState(true)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const selectedCertificate = useMemo(
    () => certificates.find((certificate) => certificate.id === selectedId) ?? null,
    [certificates, selectedId],
  )

  const load = useCallback(async (signal?: AbortSignal): Promise<void> => {
    setLoading(true)
    try {
      setCertificates(await fetchCertificates(signal))
      setErrorMessage(null)
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    void load(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 TLS 证书失败')
      }
    })
    return () => controller.abort()
  }, [load])

  useEffect(() => {
    if (!selectedId) {
      setVersions([])
      return
    }
    const controller = new AbortController()
    void fetchCertificateVersions(selectedId, controller.signal)
      .then(setVersions)
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : '加载证书版本失败')
        }
      })
    return () => controller.abort()
  }, [selectedId])

  const previewResolution = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setErrorMessage(null)
    try {
      setResolution(await resolveTlsSni(previewName))
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '解析 SNI 失败')
    }
  }

  return (
    <div className="projects-page certificates-page">
      <header className="page-header">
        <div>
          <p className="eyebrow">TLS CERTIFICATE INVENTORY</p>
          <h1>TLS</h1>
          <p className="page-description">
            leaf 证书的 DNS SAN 自动形成 ClientHello SNI 选择索引，不需要维护域名绑定规则。TLS
            握手与 HTTP Host/:authority 的 Project 选择彼此独立。当前 access-server
            尚未接入动态证书交付，控制面匹配不代表运行时已生效。
          </p>
        </div>
      </header>

      {statusError || errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage ?? statusError}</span>
        </div>
      ) : null}
      <CapabilityStrip apiState={apiState} health={health} systemStatus={systemStatus} />

      <div className="certificate-page-grid">
        <section className="settings-panel" aria-labelledby="certificate-inventory-title">
          <header>
            <div>
              <p className="eyebrow">LOGICAL CERTIFICATES</p>
              <h2 id="certificate-inventory-title">证书库存</h2>
            </div>
            <span className="status-chip status-chip-unknown">运行时交付未接入</span>
          </header>
          {loading ? (
            <div className="route-empty-state">正在加载证书…</div>
          ) : certificates.length === 0 ? (
            <div className="route-empty-state">
              <h3>还没有证书</h3>
              <p>上传首个版本后，DNS SAN 将自动成为 SNI 选择范围。</p>
            </div>
          ) : (
            <div className="certificate-list">
              {certificates.map((certificate) => (
                <article className="certificate-card" key={certificate.id}>
                  <header>
                    <div>
                      <strong>{certificate.name}</strong>
                      <small>{certificate.currentVersion.dnsNames.join(' · ')}</small>
                    </div>
                    <span
                      className={`status-chip status-chip-${certificate.currentVersion.status}`}
                    >
                      {statusLabel[certificate.currentVersion.status]}
                    </span>
                  </header>
                  <dl>
                    <div>
                      <dt>当前版本</dt>
                      <dd>V{certificate.currentVersion.version}</dd>
                    </div>
                    <div>
                      <dt>到期时间</dt>
                      <dd>{new Date(certificate.currentVersion.notAfter).toLocaleString()}</dd>
                    </div>
                    <div>
                      <dt>自动 SNI 范围</dt>
                      <dd>{certificate.currentVersion.dnsNames.length}</dd>
                    </div>
                    <div>
                      <dt>版本总数</dt>
                      <dd>{certificate.versionCount}</dd>
                    </div>
                  </dl>
                  <button
                    className="button-secondary"
                    onClick={() => setSelectedId(certificate.id)}
                    type="button"
                  >
                    更新证书版本
                  </button>
                </article>
              ))}
            </div>
          )}
        </section>

        <section className="settings-panel" aria-labelledby="certificate-upload-title">
          <header>
            <div>
              <p className="eyebrow">
                {selectedCertificate ? 'NEW IMMUTABLE VERSION' : 'NEW LOGICAL CERTIFICATE'}
              </p>
              <h2 id="certificate-upload-title">
                {selectedCertificate ? `更新 ${selectedCertificate.name}` : '创建证书'}
              </h2>
            </div>
            {selectedCertificate ? (
              <button
                className="button-secondary"
                onClick={() => setSelectedId(null)}
                type="button"
              >
                改为新建
              </button>
            ) : null}
          </header>
          <CertificateUploadForm
            certificate={selectedCertificate}
            onSaved={async (saved) => {
              await load()
              setSelectedId(saved.id)
              setVersions(await fetchCertificateVersions(saved.id))
            }}
            submitLabel={selectedCertificate ? '校验并更新当前版本' : '创建逻辑证书'}
          />
          {selectedCertificate ? (
            <div className="certificate-version-history">
              <h3>版本历史</h3>
              {versions.map((version) => (
                <div className="certificate-version-row" key={version.id}>
                  <span>V{version.version}</span>
                  <span>{statusLabel[version.status]}</span>
                  <span>{new Date(version.notAfter).toLocaleDateString()}</span>
                  <code title={version.fingerprintSha256}>
                    {version.fingerprintSha256.slice(0, 12)}…
                  </code>
                </div>
              ))}
            </div>
          ) : null}
        </section>
      </div>

      <section className="settings-panel tls-sni-panel" aria-labelledby="tls-sni-preview-title">
        <header>
          <div>
            <p className="eyebrow">CLIENTHELLO SNI → CERTIFICATE SAN</p>
            <h2 id="tls-sni-preview-title">SNI 自动解析预览</h2>
          </div>
          <span className="status-chip status-chip-unknown">控制面预览 / 未部署</span>
        </header>
        <div className="tls-sni-preview">
          <form onSubmit={(event) => void previewResolution(event)}>
            <label>
              ClientHello server name
              <input
                autoComplete="off"
                placeholder="api.example.com"
                required
                value={previewName}
                onChange={(event) => setPreviewName(event.target.value)}
              />
            </label>
            <button className="button-secondary" type="submit">
              解析
            </button>
          </form>
          {resolution ? (
            <div className="tls-sni-resolution" role="status">
              <span
                className={`status-chip status-chip-${resolution.resolutionStatus === 'matched' ? 'ready' : 'unknown'}`}
              >
                {resolutionLabel[resolution.resolutionStatus]}
              </span>
              <strong>{resolution.certificate?.name ?? '没有唯一证书'}</strong>
              {resolution.certificate ? <span>V{resolution.certificate.version}</span> : null}
              {resolution.matchKind ? <span>{resolution.matchKind}</span> : null}
              <small>运行时部署状态：unsupported</small>
            </div>
          ) : (
            <p>精确 SAN 优先于通配符 SAN；通配符只覆盖一层子域名。</p>
          )}
        </div>
      </section>
    </div>
  )
}
