import { Link } from 'react-router'

export function NotFoundPage() {
  return (
    <section className="not-found-page">
      <p className="eyebrow">404 / NOT FOUND</p>
      <h1>页面不存在</h1>
      <p>这个 Console 地址不存在，或者对应功能尚未开放。</p>
      <Link className="button-primary inline-button-link" to="/projects">
        返回 Projects
      </Link>
    </section>
  )
}
