import { isAlias, isMap, isScalar, LineCounter, parseDocument, visit } from 'yaml'

import type { ProjectRoutesModel, RouteItemModel, RouteValidationIssue } from '../api/types'

export type RouteTemplate = 'RESPONSE' | 'PROXY'

const routeFields = new Set([
  'path',
  'type',
  'service',
  'cluster',
  'addresses',
  'condition',
  'proxy_headers',
  'response_headers',
  'context',
  'rewrite',
  'status',
  'body',
  'timeout',
  'max_client_body_size',
  'max_proxy_body_size',
  'websocket_timeout',
  'flush',
  'allows',
])

const responseTemplate = `path: /healthz
type: RESPONSE
status: 200
body:
  type: TEXT
  content: ok
response_headers:
  Content-Type: text/plain; charset=utf-8
`

const proxyTemplate = `path: /api/*
type: PROXY
service: example-service/stable
timeout: 30s
rewrite: /internal
`

export interface RouteSourceAnalysis {
  path: string | null
  type: RouteTemplate | null
  condition: string | null
  issues: readonly RouteValidationIssue[]
}

const analysisCache = new WeakMap<RouteItemModel, RouteSourceAnalysis>()

export function initialRouteModel(): ProjectRoutesModel {
  return {
    schemaVersion: 2,
    kind: 'project_routes_yaml',
    routes: [],
  }
}

function createRouteId(): string {
  if (typeof globalThis.crypto?.randomUUID === 'function') return globalThis.crypto.randomUUID()
  const bytes = new Uint8Array(16)
  if (typeof globalThis.crypto?.getRandomValues === 'function') {
    globalThis.crypto.getRandomValues(bytes)
  } else {
    for (let index = 0; index < bytes.length; index += 1) {
      bytes[index] = Math.floor(Math.random() * 256)
    }
  }
  bytes[6] = (bytes[6]! & 0x0f) | 0x40
  bytes[8] = (bytes[8]! & 0x3f) | 0x80
  const hex = [...bytes].map((value) => value.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}

export function createRouteItem(template: RouteTemplate): RouteItemModel {
  return {
    id: createRouteId(),
    source: template === 'RESPONSE' ? responseTemplate : proxyTemplate,
  }
}

export function duplicateRouteItem(route: RouteItemModel): RouteItemModel {
  return { id: createRouteId(), source: route.source }
}

function position(lineCounter: LineCounter, offset: number): { line: number; column: number } {
  const value = lineCounter.linePos(offset)
  return { line: Math.max(value.line, 1), column: Math.max(value.col, 1) }
}

function fieldOffset(document: ReturnType<typeof parseDocument>, field: string): number {
  if (!isMap(document.contents)) return 0
  const pair = document.contents.items.find(
    (item) => isScalar(item.key) && item.key.value === field,
  )
  return isScalar(pair?.key) ? (pair.key.range?.[0] ?? 0) : 0
}

function sourceIssue(
  routeId: string,
  lineCounter: LineCounter,
  code: string,
  message: string,
  path: string,
  offset: number,
): RouteValidationIssue {
  return { routeId, path, ...position(lineCounter, offset), code, message }
}

function analyzeRouteSourceUncached(route: RouteItemModel): RouteSourceAnalysis {
  const lineCounter = new LineCounter()
  const document = parseDocument(route.source, {
    version: '1.2',
    schema: 'core',
    merge: false,
    resolveKnownTags: false,
    uniqueKeys: true,
    stringKeys: true,
    strict: true,
    prettyErrors: false,
    lineCounter,
  })
  const issues: RouteValidationIssue[] = document.errors.map((error) => ({
    routeId: route.id,
    path: '',
    ...position(lineCounter, error.pos[0]),
    code: error.code,
    message: error.message,
  }))
  visit(document, {
    Alias(_key, node) {
      issues.push({
        routeId: route.id,
        path: '',
        ...position(lineCounter, node.range?.[0] ?? 0),
        code: 'YAML_ALIAS_NOT_ALLOWED',
        message: 'YAML Route 不允许使用 alias',
      })
    },
    Node(_key, node) {
      if (!isAlias(node) && 'anchor' in node && node.anchor) {
        issues.push({
          routeId: route.id,
          path: '',
          ...position(lineCounter, node.range?.[0] ?? 0),
          code: 'YAML_ANCHOR_NOT_ALLOWED',
          message: 'YAML Route 不允许使用 anchor',
        })
      }
      if (node.tag) {
        issues.push({
          routeId: route.id,
          path: '',
          ...position(lineCounter, node.range?.[0] ?? 0),
          code: 'YAML_TAG_NOT_ALLOWED',
          message: 'YAML Route 不允许使用显式 tag',
        })
      }
    },
  })
  if (!isMap(document.contents)) {
    issues.push({
      routeId: route.id,
      path: '',
      line: 1,
      column: 1,
      code: 'ROUTE_ROOT_NOT_MAPPING',
      message: '每个编辑器只能包含一条 YAML mapping',
    })
    return { path: null, type: null, condition: null, issues }
  }
  if (issues.length > 0) return { path: null, type: null, condition: null, issues }
  const value: unknown = document.toJS({ maxAliasCount: 0 })
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    return { path: null, type: null, condition: null, issues }
  }
  const routeValue = value as Record<string, unknown>
  const type =
    routeValue.type === 'PROXY' || routeValue.type === 'RESPONSE' ? routeValue.type : null
  for (const field of Object.keys(routeValue)) {
    if (!routeFields.has(field)) {
      issues.push(
        sourceIssue(
          route.id,
          lineCounter,
          'UNKNOWN_ROUTE_FIELD',
          `未知 Route 字段：${field}`,
          field,
          fieldOffset(document, field),
        ),
      )
    }
  }
  if (typeof routeValue.path !== 'string' || routeValue.path.length === 0) {
    issues.push(
      sourceIssue(
        route.id,
        lineCounter,
        'INVALID_ROUTE_PATH',
        'path 必须是非空字符串',
        'path',
        fieldOffset(document, 'path'),
      ),
    )
  }
  if (!type) {
    issues.push(
      sourceIssue(
        route.id,
        lineCounter,
        'INVALID_ROUTE_TYPE',
        'type 必须是 PROXY 或 RESPONSE',
        'type',
        fieldOffset(document, 'type'),
      ),
    )
  }
  return {
    path: typeof routeValue.path === 'string' ? routeValue.path : null,
    type,
    condition: typeof routeValue.condition === 'string' ? routeValue.condition : null,
    issues,
  }
}

export function analyzeRouteSource(route: RouteItemModel): RouteSourceAnalysis {
  const cached = analysisCache.get(route)
  if (cached) return cached
  const analysis = analyzeRouteSourceUncached(route)
  analysisCache.set(route, analysis)
  return analysis
}
