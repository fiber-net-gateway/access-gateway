import { createHash } from 'node:crypto'

import { isAlias, isMap, isScalar, LineCounter, parseDocument, visit } from 'yaml'

import { canonicalJson } from '../../shared/json.js'
import type { ProjectRoutesModel, RouteItemModel } from './model.js'

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

export const ROUTE_COMPILER_REVISION = 'project-routes-yaml-v2'

export interface RouteValidationIssue {
  routeId: string
  path: string
  line: number
  column: number
  code: string
  message: string
}

export interface CompiledProjectRoutes {
  payload: Uint8Array
  payloadText: string
  sha256: string
  routes: readonly Readonly<Record<string, unknown>>[]
}

export interface ProjectRoutesCompileResult {
  compiled: CompiledProjectRoutes | null
  issues: readonly RouteValidationIssue[]
}

function position(
  lineCounter: LineCounter,
  offset: number | undefined,
): {
  line: number
  column: number
} {
  const value = lineCounter.linePos(offset ?? 0)
  return { line: Math.max(value.line, 1), column: Math.max(value.col, 1) }
}

function issue(
  route: RouteItemModel,
  lineCounter: LineCounter,
  code: string,
  message: string,
  path = '',
  offset?: number,
): RouteValidationIssue {
  return {
    routeId: route.id,
    path,
    ...position(lineCounter, offset),
    code,
    message,
  }
}

function findFieldOffset(document: ReturnType<typeof parseDocument>, field: string): number {
  if (!isMap(document.contents)) return 0
  const pair = document.contents.items.find(
    (item) => isScalar(item.key) && item.key.value === field,
  )
  return isScalar(pair?.key) ? (pair.key.range?.[0] ?? 0) : 0
}

function jsonSafe(value: unknown): boolean {
  if (value === null || typeof value === 'string' || typeof value === 'boolean') return true
  if (typeof value === 'number') return Number.isFinite(value) && Number.isSafeInteger(value)
  if (Array.isArray(value)) return value.every(jsonSafe)
  if (typeof value !== 'object') return false
  return Object.entries(value).every(([key, item]) => typeof key === 'string' && jsonSafe(item))
}

function parseRoute(route: RouteItemModel): {
  value: Readonly<Record<string, unknown>> | null
  issues: RouteValidationIssue[]
} {
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
  const issues = document.errors.map((error) =>
    issue(route, lineCounter, error.code, error.message, '', error.pos[0]),
  )
  for (const warning of document.warnings) {
    issues.push(issue(route, lineCounter, warning.code, warning.message, '', warning.pos[0]))
  }
  visit(document, {
    Alias(_key, node) {
      issues.push(
        issue(
          route,
          lineCounter,
          'YAML_ALIAS_NOT_ALLOWED',
          'YAML aliases are not allowed in route configuration',
          '',
          node.range?.[0],
        ),
      )
    },
    Node(_key, node) {
      if (!isAlias(node) && 'anchor' in node && node.anchor) {
        issues.push(
          issue(
            route,
            lineCounter,
            'YAML_ANCHOR_NOT_ALLOWED',
            'YAML anchors are not allowed in route configuration',
            '',
            node.range?.[0],
          ),
        )
      }
      if (node.tag) {
        issues.push(
          issue(
            route,
            lineCounter,
            'YAML_TAG_NOT_ALLOWED',
            'Explicit YAML tags are not allowed in route configuration',
            '',
            node.range?.[0],
          ),
        )
      }
    },
  })
  if (!isMap(document.contents)) {
    issues.push(
      issue(
        route,
        lineCounter,
        'ROUTE_ROOT_NOT_MAPPING',
        'Each route editor must contain one YAML mapping',
      ),
    )
  }
  if (issues.length > 0 || !isMap(document.contents)) return { value: null, issues }

  const parsed: unknown = document.toJS({ maxAliasCount: 0 })
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed) || !jsonSafe(parsed)) {
    return {
      value: null,
      issues: [
        issue(
          route,
          lineCounter,
          'ROUTE_VALUE_NOT_JSON_SAFE',
          'Route values must use JSON-safe strings, booleans, arrays, objects, and safe integers',
        ),
      ],
    }
  }
  const value = parsed as Record<string, unknown>
  for (const field of Object.keys(value)) {
    if (!routeFields.has(field)) {
      issues.push(
        issue(
          route,
          lineCounter,
          'UNKNOWN_ROUTE_FIELD',
          `Unknown route field: ${field}`,
          field,
          findFieldOffset(document, field),
        ),
      )
    }
  }
  if (typeof value.path !== 'string' || value.path.length === 0) {
    issues.push(
      issue(
        route,
        lineCounter,
        'INVALID_ROUTE_PATH',
        'Route path must be a non-empty string',
        'path',
        findFieldOffset(document, 'path'),
      ),
    )
  }
  if (value.type !== 'PROXY' && value.type !== 'RESPONSE') {
    issues.push(
      issue(
        route,
        lineCounter,
        'INVALID_ROUTE_TYPE',
        'Route type must be PROXY or RESPONSE',
        'type',
        findFieldOffset(document, 'type'),
      ),
    )
  }
  return { value: issues.length === 0 ? value : null, issues }
}

export function compileProjectRoutes(
  domain: string,
  model: ProjectRoutesModel,
  version = 1,
): ProjectRoutesCompileResult {
  const routes: Readonly<Record<string, unknown>>[] = []
  const issues: RouteValidationIssue[] = []
  for (const route of model.routes) {
    const parsed = parseRoute(route)
    issues.push(...parsed.issues)
    if (parsed.value) routes.push(parsed.value)
  }
  if (issues.length > 0) return { compiled: null, issues }

  const payloadText = canonicalJson({
    version,
    host: {
      [domain]: {
        https: 'S_NOT_MUST',
      },
    },
    routes,
  })
  const payload = Buffer.from(payloadText, 'utf8')
  return {
    compiled: {
      payload,
      payloadText,
      sha256: createHash('sha256').update(payload).digest('hex'),
      routes,
    },
    issues: [],
  }
}
