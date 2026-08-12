import { useState, type FormEvent } from 'react'

import { createCertificate } from '../api/client'
import type { CertificateView } from '../api/types'

interface CertificateUploadFormProps {
  onCreated(certificate: CertificateView): Promise<void> | void
  submitLabel?: string
}

export function CertificateUploadForm({
  onCreated,
  submitLabel = '上传到证书库存',
}: CertificateUploadFormProps) {
  const [name, setName] = useState('')
  const [certificatePem, setCertificatePem] = useState('')
  const [privateKeyPem, setPrivateKeyPem] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const submit = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setSubmitting(true)
    setErrorMessage(null)
    try {
      const created = await createCertificate({ name, certificatePem, privateKeyPem })
      setName('')
      setCertificatePem('')
      setPrivateKeyPem('')
      await onCreated(created)
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
        <span>服务端会校验证书链、有效期、DNS SAN 和私钥匹配。</span>
        <button className="button-primary" disabled={submitting} type="submit">
          {submitting ? '校验并上传中…' : submitLabel}
        </button>
      </div>
    </form>
  )
}
