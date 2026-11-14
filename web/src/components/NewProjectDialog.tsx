import { useState, type FormEvent } from 'react'

import { createProject } from '../api/client'

interface NewProjectDialogProps {
  open: boolean
  onClose(): void
  onCreated(project: { id: string }): void
}

export function NewProjectDialog({ open, onClose, onCreated }: NewProjectDialogProps) {
  const [domain, setDomain] = useState('')
  const [creating, setCreating] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  if (!open) return null

  const submit = async (event: FormEvent): Promise<void> => {
    event.preventDefault()
    setCreating(true)
    setErrorMessage(null)
    try {
      const created = await createProject(domain)
      setDomain('')
      onCreated(created)
    } catch (error: unknown) {
      setErrorMessage(error instanceof Error ? error.message : '创建 Project 失败')
    } finally {
      setCreating(false)
    }
  }

  return (
    <div className="dialog-backdrop" role="presentation">
      <form
        aria-labelledby="new-project-title"
        className="dialog-card"
        onSubmit={(event) => void submit(event)}
        role="dialog"
      >
        <p className="eyebrow">CREATE DOMAIN PROJECT</p>
        <h2 id="new-project-title">新建域名 Project</h2>
        <p>每个 Project 拥有一个不可变主域名,创建后进入路由工作区。</p>
        <label>
          主域名
          <input
            autoFocus
            autoComplete="off"
            placeholder="api.example.com"
            required
            value={domain}
            onChange={(event) => setDomain(event.target.value)}
          />
        </label>
        {errorMessage ? (
          <p className="dialog-error" role="alert">
            {errorMessage}
          </p>
        ) : null}
        <div className="dialog-actions">
          <button className="button-secondary" disabled={creating} onClick={onClose} type="button">
            取消
          </button>
          <button className="button-primary" disabled={creating} type="submit">
            {creating ? '创建中…' : '创建并进入'}
          </button>
        </div>
      </form>
    </div>
  )
}
