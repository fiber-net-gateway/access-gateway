import { useCallback, useEffect, useState } from 'react'

import {
  bindProjectCertificate,
  fetchCertificates,
  fetchProjectCertificate,
  unbindProjectCertificate,
} from '../api/client'
import type { CertificateView, ProjectCertificateBindingView } from '../api/types'
import { CertificateUploadForm } from '../components/CertificateUploadForm'
import { useProjectContext } from './ProjectLayout'

export function ProjectCertificatePage() {
  const { project } = useProjectContext()
  const [certificates, setCertificates] = useState<readonly CertificateView[]>([])
  const [binding, setBinding] = useState<ProjectCertificateBindingView | null>(null)
  const [selectedId, setSelectedId] = useState('')
  const [busy, setBusy] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const load = useCallback(
    async (signal?: AbortSignal): Promise<void> => {
      const [items, current] = await Promise.all([
        fetchCertificates(signal),
        fetchProjectCertificate(project.id, signal),
      ])
      setCertificates(items)
      setBinding(current)
      setSelectedId(current.certificate?.id ?? '')
    },
    [project.id],
  )

  useEffect(() => {
    const controller = new AbortController()
    void load(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 Project 证书失败')
      }
    })
    return () => controller.abort()
  }, [load])

  const bind = async (certificateId = selectedId): Promise<void> => {
    if (!certificateId) return
    setBusy(true)
    setErrorMessage(null)
    try {
      setBinding(await bindProjectCertificate(project.id, certificateId))
      setSelectedId(certificateId)
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '证书绑定失败')
    } finally {
      setBusy(false)
    }
  }

  const unbind = async (): Promise<void> => {
    if (!window.confirm(`确定解除 ${project.domain} 的证书绑定吗？`)) return
    setBusy(true)
    try {
      setBinding(await unbindProjectCertificate(project.id))
      setSelectedId('')
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '解除绑定失败')
    } finally {
      setBusy(false)
    }
  }

  return (
    <section className="project-subpage" aria-labelledby="project-certificate-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT / CERTIFICATE</p>
          <h2 id="project-certificate-title">Certificate</h2>
          <p>证书绑定与配置版本相互独立；私钥不会进入 Route payload 或 rnacos。</p>
        </div>
        <span className="status-chip status-chip-unknown">运行时部署未接入</span>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>操作未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      <div className="project-settings-grid">
        <section className="settings-panel">
          <header>
            <div>
              <p className="eyebrow">CURRENT BINDING</p>
              <h3>{binding?.certificate ? binding.certificate.name : '未绑定证书'}</h3>
            </div>
            <span
              className={`status-chip status-chip-${binding?.coverageStatus === 'covered' ? 'ready' : 'unknown'}`}
            >
              {binding?.coverageStatus === 'covered' ? 'SAN 已覆盖' : '未绑定'}
            </span>
          </header>
          {binding?.certificate ? (
            <div className="certificate-binding-detail">
              <p>{binding.certificate.dnsNames.join(' · ')}</p>
              <dl>
                <div>
                  <dt>有效期至</dt>
                  <dd>{new Date(binding.certificate.notAfter).toLocaleString()}</dd>
                </div>
                <div>
                  <dt>指纹</dt>
                  <dd>{binding.certificate.fingerprintSha256}</dd>
                </div>
                <div>
                  <dt>实例状态</dt>
                  <dd>动态证书部署未接入</dd>
                </div>
              </dl>
            </div>
          ) : (
            <p className="settings-description">选择一个 DNS SAN 覆盖当前域名的有效证书。</p>
          )}
          <div className="binding-controls">
            <label>
              证书库存
              <select value={selectedId} onChange={(event) => setSelectedId(event.target.value)}>
                <option value="">请选择证书</option>
                {certificates.map((certificate) => (
                  <option
                    disabled={!['valid', 'expiring'].includes(certificate.status)}
                    key={certificate.id}
                    value={certificate.id}
                  >
                    {certificate.name} · {certificate.dnsNames.join(', ')}
                  </option>
                ))}
              </select>
            </label>
            <button
              className="button-primary"
              disabled={busy || !selectedId || selectedId === binding?.certificate?.id}
              onClick={() => void bind()}
              type="button"
            >
              {busy ? '处理中…' : '绑定证书'}
            </button>
            {binding?.certificate ? (
              <button
                className="button-secondary"
                disabled={busy}
                onClick={() => void unbind()}
                type="button"
              >
                解除绑定
              </button>
            ) : null}
          </div>
        </section>

        <section className="settings-panel">
          <header>
            <div>
              <p className="eyebrow">UPLOAD AND BIND</p>
              <h3>上传新证书版本</h3>
            </div>
          </header>
          <CertificateUploadForm
            onCreated={async (created) => {
              setCertificates((current) => [...current, created])
              await bind(created.id)
            }}
            submitLabel="上传并绑定"
          />
        </section>
      </div>
    </section>
  )
}
