import { useEffect, useRef, useState } from 'react'
import { useLocation, useNavigate } from 'react-router'

import { fetchProjects } from '../api/client'
import type { ProjectView } from '../api/types'

interface ProjectSwitcherProps {
  current: ProjectView
}

// Sidebar header that shows the current project domain as a heading and opens
// a searchable dropdown to jump to the same tab of another project.
export function ProjectSwitcher({ current }: ProjectSwitcherProps) {
  const [open, setOpen] = useState(false)
  const [query, setQuery] = useState('')
  const [projects, setProjects] = useState<readonly ProjectView[] | null>(null)
  const [loading, setLoading] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const navigate = useNavigate()
  const location = useLocation()
  const rootRef = useRef<HTMLDivElement | null>(null)

  useEffect(() => {
    if (!open || projects) return
    const controller = new AbortController()
    setLoading(true)
    fetchProjects(controller.signal)
      .then((items) => {
        setProjects(items)
        setErrorMessage(null)
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted)
          setErrorMessage(error instanceof Error ? error.message : '加载项目列表失败')
      })
      .finally(() => {
        if (!controller.signal.aborted) setLoading(false)
      })
    return () => controller.abort()
  }, [open, projects])

  useEffect(() => {
    if (!open) return
    const onPointerDown = (event: MouseEvent): void => {
      if (rootRef.current && !rootRef.current.contains(event.target as Node)) setOpen(false)
    }
    const onKeyDown = (event: KeyboardEvent): void => {
      if (event.key === 'Escape') setOpen(false)
    }
    document.addEventListener('mousedown', onPointerDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('mousedown', onPointerDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [open])

  const normalized = query.trim().toLowerCase()
  const candidates = (projects ?? []).filter((item) => item.id !== current.id)
  const filtered = normalized
    ? candidates.filter((item) => item.domain.toLowerCase().includes(normalized))
    : candidates

  const switchTo = (target: ProjectView): void => {
    setOpen(false)
    setQuery('')
    void navigate({
      pathname: location.pathname.replace(`/projects/${current.id}`, `/projects/${target.id}`),
      search: '',
    })
  }

  return (
    <div className="project-switcher" ref={rootRef}>
      <button
        aria-expanded={open}
        aria-haspopup="listbox"
        className="project-switcher-button"
        onClick={() => setOpen((value) => !value)}
        type="button"
      >
        <h2 className="project-switcher-title">{current.domain}</h2>
        <span aria-hidden="true">▾</span>
      </button>

      {open ? (
        <div className="project-switcher-panel" role="listbox" aria-label="切换 Project">
          <input
            autoFocus
            className="project-switcher-search"
            placeholder="搜索域名…"
            type="search"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
          />
          {errorMessage ? <p className="project-switcher-error">{errorMessage}</p> : null}
          {loading ? (
            <p className="project-switcher-hint">正在加载…</p>
          ) : filtered.length === 0 ? (
            <p className="project-switcher-hint">
              {candidates.length === 0 ? '没有其他项目' : '没有匹配的项目'}
            </p>
          ) : (
            <ul>
              {filtered.map((item) => (
                <li key={item.id}>
                  <button onClick={() => switchTo(item)} role="option" type="button">
                    <span aria-selected={false}>{item.domain}</span>
                    <small>
                      {item.publishedVersion ? `已发布 V${item.publishedVersion}` : '尚未发布'}
                    </small>
                  </button>
                </li>
              ))}
            </ul>
          )}
        </div>
      ) : null}
    </div>
  )
}
