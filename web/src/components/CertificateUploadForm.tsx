import { useEffect, useState, type FormEvent } from 'react'

import { createCertificate, createCertificateVersion } from '../api/client'
import type { CertificateView } from '../api/types'

interface CertificateUploadFormProps {
  certificate?: CertificateView | null
  onSaved(certificate: CertificateView): Promise<void> | void
  submitLabel?: string
}

export function CertificateUploadForm({
  certificate = null,
  onSaved,
  submitLabel = '上传到证书库存',
}: CertificateUploadFormProps) {
  const [name, setName] = useState('')
  const [certificatePem, setCertificatePem] = useState('')
  const [privateKeyPem, setPrivateKeyPem] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  useEffect(() => {
    setName(certificate?.name ?? '')
    setCertificatePem('')
    setPrivateKeyPem('')
    setErrorMessage(null)
  }, [certificate])

  const submit = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setSubmitting(true)
    setErrorMessage(null)
    try {
      const saved = certificate
        ? await createCertificateVersion(certificate.id, {
            certificatePem,
            privateKeyPem,
            lockVersion: certificate.lockVersion,
          })
        : await createCertificate({ name, certificatePem, privateKeyPem })
      if (!certificate) setName('')
      setCertificatePem('')
      setPrivateKeyPem('')
      await onSaved(saved)
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : '证书上传失败')
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <form className="certificate-upload-form" onSubmit={(event) => void submit(event)}>
      <label>
        显示名称
        <input
          autoComplete="off"
          maxLength={255}
          placeholder="api.example.com / 2026"
          readOnly={certificate !== null}
          required
          value={name}
          onChange={(event) => setName(event.target.value)}
        />
      </label>
      <div className="certificate-pem-grid">
        <label>
          PEM 证书链（leaf 在前）
          <textarea
            aria-label="PEM 证书链"
            placeholder="-----BEGIN CERTIFICATE-----"
            required
            rows={8}
            spellCheck={false}
            value={certificatePem}
            onChange={(event) => setCertificatePem(event.target.value)}
          />
        </label>
        <label>
          PEM 私钥（不会回显或提供下载）
          <textarea
            aria-label="PEM 私钥"
            autoComplete="off"
            placeholder="-----BEGIN PRIVATE KEY-----"
            required
            rows={8}
            spellCheck={false}
            value={privateKeyPem}
            onChange={(event) => setPrivateKeyPem(event.target.value)}
          />
        </label>
      </div>
      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>上传未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}
      <div className="form-actions">
        <span>
          {certificate
            ? '新版本必须继续覆盖该逻辑证书管理的全部域名。'
            : '首个版本的 DNS SAN 将成为稳定的自动匹配范围。'}
        </span>
        <button className="button-primary" disabled={submitting} type="submit">
          {submitting ? '校验并上传中…' : submitLabel}
        </button>
      </div>
    </form>
  )
}
