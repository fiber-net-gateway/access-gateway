import { Link, NavLink, useParams } from 'react-router'

import {
  documentationContent,
  documentationLanguageLabels,
  documentationLanguages,
  documentationMetadata,
  documentationPath,
  documentationTopics,
  isDocumentationLanguage,
  isDocumentationTopic,
} from '../docs/documentation'

export function DocumentationPage() {
  const { language: languageParam, topic: topicParam } = useParams()

  if (!isDocumentationLanguage(languageParam) || !isDocumentationTopic(topicParam)) {
    return (
      <section className="not-found-page">
        <p className="eyebrow">DOCUMENTATION</p>
        <h1>文档页面不存在 / Documentation not found</h1>
        <p>请选择有效的语言和主题。 Select an available language and topic.</p>
        <Link className="button-primary inline-button-link" to="/docs/zh-CN/routing">
          打开文档 / Open documentation
        </Link>
      </section>
    )
  }

  const language = languageParam
  const topic = topicParam
  const metadata = documentationMetadata[language]

  return (
    <section className="documentation-page">
      <header className="documentation-toolbar">
        <div>
          <p className="eyebrow">DOCUMENTATION</p>
          <p className="documentation-console-title">{metadata.consoleTitle}</p>
          <p className="documentation-console-description">{metadata.consoleDescription}</p>
        </div>

        <nav
          className="documentation-language-navigation"
          aria-label={metadata.languageNavigationLabel}
        >
          {documentationLanguages.map((candidate) => (
            <NavLink
              aria-current={candidate === language ? 'page' : undefined}
              className={candidate === language ? 'active' : undefined}
              key={candidate}
              lang={candidate}
              to={documentationPath(candidate, topic)}
            >
              {documentationLanguageLabels[candidate]}
            </NavLink>
          ))}
        </nav>
      </header>

      <div className="documentation-layout">
        <aside className="documentation-sidebar">
          <nav aria-label={metadata.topicNavigationLabel}>
            {documentationTopics.map((candidate) => (
              <NavLink
                aria-current={candidate === topic ? 'page' : undefined}
                className={candidate === topic ? 'active' : undefined}
                key={candidate}
                to={documentationPath(language, candidate)}
              >
                <strong>{metadata.topics[candidate].label}</strong>
                <small>{metadata.topics[candidate].detail}</small>
              </NavLink>
            ))}
          </nav>
          <p>{metadata.sourceNote}</p>
        </aside>

        <article className="documentation-article" lang={language}>
          {/* The build script escapes raw HTML and rejects unsafe link protocols before generating this fragment. */}
          <div
            className="documentation-markdown"
            dangerouslySetInnerHTML={{ __html: documentationContent(language, topic) }}
          />
        </article>
      </div>
    </section>
  )
}
