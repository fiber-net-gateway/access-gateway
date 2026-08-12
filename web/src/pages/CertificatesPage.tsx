import { useCallback, useEffect, useMemo, useState } from 'react'

import { fetchCertificates, fetchCertificateVersions } from '../api/client'
import type { CertificateVersionView, CertificateView } from '../api/types'
import { CapabilityStrip } from '../components/CapabilityStrip'
import { CertificateUploadForm } from '../components/CertificateUploadForm'
import { useConsoleContext } from '../App'

const statusLabel: Record<CertificateVersionView['status'], string> = {
  valid: '有效',
  expiring: '即将到期',
  expired: '已过期',
  superseded: '历史版本',
}

export function CertificatesPage() {
  const { apiState, health, systemStatus, statusError } = useConsoleContext()
  const [certificates, setCertificates] = useState<readonly CertificateView[]>([])
  const [selectedId, setSelectedId] = useState<string | null>(null)
  const [versions, setVersions] = useState<readonly CertificateVersionView[]>([])
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
        setErrorMessage(error instanceof Error ? error.message : '加载证书库存失败')
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

  return (
    <div className="projects-page certificates-page">
      <header className="page-header">
        <div>
          <p className="eyebrow">TLS CERTIFICATE INVENTORY</p>
          <h1>Certificates</h1>
          <p className="page-description">
            逻辑证书按 DNS 名称自动匹配 Project；续期只新增一个不可变版本，不需要逐域名切换。 当前
            access-server 尚未接入动态 SNI，自动匹配不代表运行时生效。
          </p>
        </div>
      </header>

      {statusError || errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>部分信息加载失败</strong>
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
              <p>上传首个版本后，DNS SAN 将成为该逻辑证书的稳定自动匹配范围。</p>
            </div>
          ) : (
            <div className="certificate-list">
              {certificates.map((certificate) => (
                <article className="certificate-card" key={certificate.id}>
                  <header>
                    <div>
                      <strong>{certificate.name}</strong>
                      <small>{certificate.managedDnsNames.join(' · ')}</small>
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
                      <dt>自动匹配 Project</dt>
                      <dd>{certificate.matchedProjectCount}</dd>
                    </div>
                    <div>
                      <dt>历史版本数</dt>
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
    </div>
  )
}
