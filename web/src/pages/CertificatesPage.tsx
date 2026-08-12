import { useCallback, useEffect, useState } from 'react'

import { fetchCertificates } from '../api/client'
import type { CertificateView } from '../api/types'
import { CapabilityStrip } from '../components/CapabilityStrip'
import { CertificateUploadForm } from '../components/CertificateUploadForm'
import { useConsoleContext } from '../App'

const statusLabel: Record<CertificateView['status'], string> = {
  valid: '有效',
  expiring: '即将到期',
  expired: '已过期',
  superseded: '已被替换',
}

export function CertificatesPage() {
  const { apiState, health, systemStatus, statusError } = useConsoleContext()
  const [certificates, setCertificates] = useState<readonly CertificateView[]>([])
  const [loading, setLoading] = useState(true)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

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

  return (
    <div className="projects-page certificates-page">
      <header className="page-header">
        <div>
          <p className="eyebrow">TLS CERTIFICATE INVENTORY</p>
          <h1>Certificates</h1>
          <p className="page-description">
            管理不可变证书版本与域名绑定。当前 access-server 尚未接入动态
            SNI，绑定不代表运行时生效。
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
              <p className="eyebrow">INVENTORY</p>
              <h2 id="certificate-inventory-title">证书库存</h2>
            </div>
            <span className="status-chip status-chip-unknown">运行时交付未接入</span>
          </header>
          {loading ? (
            <div className="route-empty-state">正在加载证书…</div>
          ) : certificates.length === 0 ? (
            <div className="route-empty-state">
              <h3>还没有证书</h3>
              <p>上传 PEM 证书链和匹配私钥后，可在 Project 中完成绑定。</p>
            </div>
          ) : (
            <div className="certificate-list">
              {certificates.map((certificate) => (
                <article className="certificate-card" key={certificate.id}>
                  <header>
                    <div>
                      <strong>{certificate.name}</strong>
                      <small>{certificate.dnsNames.join(' · ')}</small>
                    </div>
                    <span className={`status-chip status-chip-${certificate.status}`}>
                      {statusLabel[certificate.status]}
                    </span>
                  </header>
                  <dl>
                    <div>
                      <dt>到期时间</dt>
                      <dd>{new Date(certificate.notAfter).toLocaleString()}</dd>
                    </div>
                    <div>
                      <dt>绑定数</dt>
                      <dd>{certificate.bindingCount}</dd>
                    </div>
                    <div>
                      <dt>SHA-256</dt>
                      <dd title={certificate.fingerprintSha256}>
                        {certificate.fingerprintSha256.slice(0, 20)}…
                      </dd>
                    </div>
                  </dl>
                </article>
              ))}
            </div>
          )}
        </section>

        <section className="settings-panel" aria-labelledby="certificate-upload-title">
          <header>
            <div>
              <p className="eyebrow">NEW IMMUTABLE VERSION</p>
              <h2 id="certificate-upload-title">上传证书</h2>
            </div>
          </header>
          <CertificateUploadForm onCreated={async () => load()} />
        </section>
      </div>
    </div>
  )
}
