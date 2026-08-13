import { createHash } from 'node:crypto'
import { isIP } from 'node:net'

import { isAlias, isMap, isScalar, LineCounter, parseDocument, visit } from 'yaml'

import { canonicalJson } from '../../shared/json.js'
import type { HttpsRedirect, ProjectRoutesModel, RouteItemModel } from './model.js'

const routeFields = new Set([
  'path',
  'method',
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

export const ROUTE_COMPILER_REVISION = 'project-routes-mixed-v5-method-script'

const networkPolicyRouteId = '00000000-0000-4000-8000-000000000099'

const httpsStrategies: Readonly<Record<HttpsRedirect, string>> = {
  off: 'S_NOT_MUST',
  '301': 'S_301',
  '302': 'S_302',
  '307': 'S_307',
  '308': 'S_308',
}

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

function isScalarValue(value: unknown): boolean {
  return (
    value === null ||
    typeof value === 'string' ||
    typeof value === 'boolean' ||
    typeof value === 'number'
  )
}

function isScalarMap(value: unknown): boolean {
  return (
    value === null ||
    (typeof value === 'object' &&
      !Array.isArray(value) &&
      Object.values(value).every(isScalarValue))
  )
}

function isScalarList(value: unknown): boolean {
  return value === null || (Array.isArray(value) && value.every(isScalarValue))
}

function validateRouteFieldShapes(
  route: RouteItemModel,
  lineCounter: LineCounter,
  document: ReturnType<typeof parseDocument>,
  value: Readonly<Record<string, unknown>>,
  issues: RouteValidationIssue[],
): void {
  const addTypeIssue = (field: string, expected: string): void => {
    issues.push(
      issue(
        route,
        lineCounter,
        'INVALID_ROUTE_FIELD_TYPE',
        `${field} must be ${expected}`,
        field,
        findFieldOffset(document, field),
      ),
    )
  }

  for (const field of ['proxy_headers', 'response_headers', 'context']) {
    if (field in value && !isScalarMap(value[field])) addTypeIssue(field, 'an object or null')
  }
  for (const field of ['addresses', 'allows']) {
    if (field in value && !isScalarList(value[field])) addTypeIssue(field, 'an array or null')
  }
  for (const field of [
    'method',
    'service',
    'cluster',
    'condition',
    'rewrite',
    'status',
    'timeout',
    'max_client_body_size',
    'max_proxy_body_size',
    'websocket_timeout',
    'flush',
  ]) {
    if (field in value && !isScalarValue(value[field])) {
      addTypeIssue(field, 'a scalar value or null')
    }
  }
  if ('body' in value && !isScalarMap(value.body)) addTypeIssue('body', 'an object or null')
}

const httpMethodPattern = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/u

function validateMethod(
  route: RouteItemModel,
  value: unknown,
  lineCounter: LineCounter,
  offset = 0,
): RouteValidationIssue | null {
  if (value === undefined || value === null) return null
  if (typeof value !== 'string' || value.length > 64 || !httpMethodPattern.test(value)) {
    return issue(
      route,
      lineCounter,
      'INVALID_ROUTE_METHOD',
      'Route method must be a 1-64 byte HTTP token',
      'method',
      offset,
    )
  }
  return null
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
  const methodIssue = validateMethod(
    route,
    value.method,
    lineCounter,
    findFieldOffset(document, 'method'),
  )
  if (methodIssue) issues.push(methodIssue)
  validateRouteFieldShapes(route, lineCounter, document, value, issues)
  return { value: issues.length === 0 ? value : null, issues }
}

function validateNetworkPolicy(model: ProjectRoutesModel): RouteValidationIssue[] {
  const issues: RouteValidationIssue[] = []
  const all = [
    ...model.networkPolicy.allowedCidrs.map((value, index) => ({
      value,
      path: `networkPolicy.allowedCidrs.${index}`,
    })),
    ...model.networkPolicy.deniedCidrs.map((value, index) => ({
      value,
      path: `networkPolicy.deniedCidrs.${index}`,
    })),
  ]
  const seen = new Set<string>()
  for (const { value, path } of all) {
    const slash = value.indexOf('/')
    const address = slash === -1 ? value : value.slice(0, slash)
    const prefixText = slash === -1 ? null : value.slice(slash + 1)
    const family = isIP(address)
    const maximumPrefix = family === 4 ? 32 : family === 6 ? 128 : 0
    const prefix = prefixText !== null && /^\d+$/u.test(prefixText) ? Number(prefixText) : null
    if (
      value !== value.trim() ||
      value.startsWith('!') ||
      family === 0 ||
      (prefixText !== null &&
        (prefix === null || !Number.isSafeInteger(prefix) || prefix < 0 || prefix > maximumPrefix))
    ) {
      issues.push({
        routeId: networkPolicyRouteId,
        path,
        line: 1,
        column: 1,
        code: 'INVALID_NETWORK_POLICY_CIDR',
        message: `${value} is not a valid IPv4 or IPv6 CIDR`,
      })
      continue
    }
    const key = value.toLowerCase()
    if (seen.has(key)) {
      issues.push({
        routeId: networkPolicyRouteId,
        path,
        line: 1,
        column: 1,
        code: 'DUPLICATE_NETWORK_POLICY_CIDR',
        message: `${value} is duplicated in the network policy`,
      })
    }
    seen.add(key)
  }
  return issues
}

export function compileProjectRoutes(
  domain: string,
  model: ProjectRoutesModel,
  version = 1,
): ProjectRoutesCompileResult {
  const routes: Readonly<Record<string, unknown>>[] = []
  const issues: RouteValidationIssue[] = validateNetworkPolicy(model)
  for (const route of model.routes) {
    const parsed =
      route.format === 'yaml'
        ? parseRoute(route)
        : (() => {
            const lineCounter = new LineCounter()
            const routeIssues: RouteValidationIssue[] = []
            if (route.path.length === 0 || route.path.length > 2048) {
              routeIssues.push(
                issue(
                  route,
                  lineCounter,
                  'INVALID_ROUTE_PATH',
                  'JavaScript Route path must contain 1-2048 characters',
                  'path',
                ),
              )
            }
            const methodIssue = validateMethod(route, route.method, lineCounter)
            if (methodIssue) routeIssues.push(methodIssue)
            if (route.source.trim().length === 0) {
              routeIssues.push(
                issue(
                  route,
                  lineCounter,
                  'EMPTY_ROUTE_SCRIPT',
                  'JavaScript Route source must not be empty',
                  'source',
                ),
              )
            }
            return {
              value:
                routeIssues.length === 0
                  ? {
                      path: route.path,
                      ...(route.method ? { method: route.method } : {}),
                      type: 'SCRIPT',
                      script: route.source,
                    }
                  : null,
              issues: routeIssues,
            }
          })()
    issues.push(...parsed.issues)
    if (parsed.value) {
      if (model.networkPolicy.source === 'project' && 'allows' in parsed.value) {
        issues.push({
          routeId: route.id,
          path: 'allows',
          line: 1,
          column: 1,
          code: 'ROUTE_NETWORK_POLICY_CONFLICT',
          message: 'Route allows must be removed while the project network policy is authoritative',
        })
      } else if (model.networkPolicy.source === 'project') {
        const allows = [
          ...model.networkPolicy.allowedCidrs,
          ...model.networkPolicy.deniedCidrs.map((cidr) => `!${cidr}`),
        ]
        routes.push(allows.length > 0 ? { ...parsed.value, allows } : parsed.value)
      } else {
        routes.push(parsed.value)
      }
    }
  }
  if (issues.length > 0) return { compiled: null, issues }

  const payloadText = canonicalJson({
    version,
    host: {
      [domain]: {
        https: httpsStrategies[model.networkPolicy.httpsRedirect],
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
