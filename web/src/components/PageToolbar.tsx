import type { ReactNode } from 'react'

interface PageToolbarProps {
  title: ReactNode
  titleId?: string
  badges?: ReactNode
  meta?: ReactNode
  actions?: ReactNode
}

// Sticky per-page toolbar: one row carrying the page title, workspace badges
// (baseline version / dirty state) and the primary actions. It stays visible
// while the page content scrolls.
export function PageToolbar({ title, titleId, badges, meta, actions }: PageToolbarProps) {
  return (
    <header className="page-toolbar">
      <div className="page-toolbar-lead">
        <h2 className="page-toolbar-title" id={titleId}>
          {title}
        </h2>
        {badges ? <div className="page-toolbar-badges">{badges}</div> : null}
      </div>
      <div className="page-toolbar-side">
        {meta ? <div className="page-toolbar-meta">{meta}</div> : null}
        {actions ? <div className="header-actions">{actions}</div> : null}
      </div>
    </header>
  )
}
