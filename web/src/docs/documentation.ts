import enRoutingHtml from '../../../docs/user-guide/html/en/routing.html?raw'
import enScriptReferenceHtml from '../../../docs/user-guide/html/en/script-reference.html?raw'
import enScriptRoutingHtml from '../../../docs/user-guide/html/en/script-routing.html?raw'
import zhCnRoutingHtml from '../../../docs/user-guide/html/zh-CN/routing.html?raw'
import zhCnScriptReferenceHtml from '../../../docs/user-guide/html/zh-CN/script-reference.html?raw'
import zhCnScriptRoutingHtml from '../../../docs/user-guide/html/zh-CN/script-routing.html?raw'

export const documentationLanguages = ['zh-CN', 'en'] as const
export const documentationTopics = ['routing', 'script-routing', 'script-reference'] as const

export type DocumentationLanguage = (typeof documentationLanguages)[number]
export type DocumentationTopic = (typeof documentationTopics)[number]

interface DocumentationTopicMetadata {
  label: string
  detail: string
}

interface DocumentationLocaleMetadata {
  consoleTitle: string
  consoleDescription: string
  languageNavigationLabel: string
  topicNavigationLabel: string
  sourceNote: string
  topics: Record<DocumentationTopic, DocumentationTopicMetadata>
}

export const documentationLanguageLabels: Record<DocumentationLanguage, string> = {
  'zh-CN': '简体中文',
  en: 'English',
}

export const documentationMetadata: Record<DocumentationLanguage, DocumentationLocaleMetadata> = {
  'zh-CN': {
    consoleTitle: 'Access Gateway 使用文档',
    consoleDescription: '路由配置、脚本路由和运行时 API 的版本化参考。',
    languageNavigationLabel: '文档语言',
    topicNavigationLabel: '文档主题',
    sourceNote:
      '内容从 docs/user-guide 的 Markdown 生成；Native Validator 是配置能力的最终判定者。',
    topics: {
      routing: { label: '路由规则', detail: 'Host、Path、策略、RESPONSE 与 PROXY' },
      'script-routing': { label: '脚本路由用法', detail: '创建、执行、请求体和响应示例' },
      'script-reference': { label: '脚本与 API 参考', detail: '语法、标准库、req/resp 与常量' },
    },
  },
  en: {
    consoleTitle: 'Access Gateway documentation',
    consoleDescription: 'Versioned guidance for routes, script Routes, and runtime APIs.',
    languageNavigationLabel: 'Documentation language',
    topicNavigationLabel: 'Documentation topics',
    sourceNote:
      'Generated from Markdown in docs/user-guide; Native Validator remains the final capability authority.',
    topics: {
      routing: { label: 'Route rules', detail: 'Host, Path, policy, RESPONSE, and PROXY' },
      'script-routing': {
        label: 'Script Route usage',
        detail: 'Creation, execution, bodies, and responses',
      },
      'script-reference': {
        label: 'Script and API reference',
        detail: 'Syntax, standard library, req/resp, constants',
      },
    },
  },
}

const documentationHtml: Record<DocumentationLanguage, Record<DocumentationTopic, string>> = {
  'zh-CN': {
    routing: zhCnRoutingHtml,
    'script-routing': zhCnScriptRoutingHtml,
    'script-reference': zhCnScriptReferenceHtml,
  },
  en: {
    routing: enRoutingHtml,
    'script-routing': enScriptRoutingHtml,
    'script-reference': enScriptReferenceHtml,
  },
}

export function isDocumentationLanguage(value: string | undefined): value is DocumentationLanguage {
  return documentationLanguages.some((language) => language === value)
}

export function isDocumentationTopic(value: string | undefined): value is DocumentationTopic {
  return documentationTopics.some((topic) => topic === value)
}

export function documentationPath(
  language: DocumentationLanguage,
  topic: DocumentationTopic,
): string {
  return `/docs/${language}/${topic}`
}

export function documentationContent(
  language: DocumentationLanguage,
  topic: DocumentationTopic,
): string {
  return documentationHtml[language][topic]
}
