import { createHash } from 'node:crypto'
import { isIP } from 'node:net'

import { isAlias, isMap, isScalar, LineCounter, parseDocument, visit } from 'yaml'

import {
  fallbackAccessConfigLimits,
  utf8Bytes,
} from '../../integrations/native-validator/limits.js'
import type { AccessConfigLimits } from '../../integrations/native-validator/model.js'
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
  'gzip',
  'timeout',
  'max_client_body_size',
  'max_proxy_body_size',
  'websocket_timeout',
  'flush',
  'allows',
])

export const ROUTE_COMPILER_REVISION = 'project-routes-mixed-v6-native-limits'

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

function limitIssue(
  routeId: string,
  path: string,
  resource: string,
  maximum: number,
): RouteValidationIssue {
  return {
    routeId,
    path,
    line: 1,
    column: 1,
    code: 'limit_exceeded',
    message: `${resource} exceeds the native limit of ${maximum}`,
  }
}

function validateModelLimits(
  domain: string,
  model: ProjectRoutesModel,
  limits: AccessConfigLimits,
): RouteValidationIssue[] {
  const routeLimits = limits.projectRoute
  const fallbackRouteId = model.routes[0]?.id ?? networkPolicyRouteId
  if (utf8Bytes(domain) > routeLimits.maxHostPatternBytes) {
    return [limitIssue(fallbackRouteId, 'host', 'Host pattern', routeLimits.maxHostPatternBytes)]
  }
  if (model.routes.length > routeLimits.maxRoutes) {
    return [limitIssue(fallbackRouteId, 'routes', 'Route count', routeLimits.maxRoutes)]
  }
  const cidrs = [
    ...model.networkPolicy.allowedCidrs.map((value, index) => ({
      value,
      path: `networkPolicy.allowedCidrs.${index}`,
    })),
    ...model.networkPolicy.deniedCidrs.map((value, index) => ({
      value,
      path: `networkPolicy.deniedCidrs.${index}`,
    })),
  ]
  if (cidrs.length > routeLimits.maxCidrsPerRoute) {
    return [
      limitIssue(
        networkPolicyRouteId,
        'networkPolicy',
        'Project network policy CIDR count',
        routeLimits.maxCidrsPerRoute,
      ),
    ]
  }
  for (const cidr of cidrs) {
    if (utf8Bytes(cidr.value) > routeLimits.maxCidrBytes) {
      return [
        limitIssue(
          networkPolicyRouteId,
          cidr.path,
          'Project network policy CIDR',
          routeLimits.maxCidrBytes,
        ),
      ]
    }
  }

  let totalSourceBytes = 0
  for (const route of model.routes) {
    const sourceBytes = utf8Bytes(route.source)
    const sourceLimit =
      route.format === 'js' ? routeLimits.maxScriptBytes : routeLimits.maxPayloadBytes
    if (sourceBytes > sourceLimit) {
      return [limitIssue(route.id, 'source', 'Route source', sourceLimit)]
    }
    totalSourceBytes += sourceBytes
    if (totalSourceBytes > routeLimits.maxPayloadBytes) {
      return [limitIssue(route.id, 'source', 'Combined Route source', routeLimits.maxPayloadBytes)]
    }
    if (route.format === 'js') {
      if (utf8Bytes(route.path) > routeLimits.maxPathBytes) {
        return [limitIssue(route.id, 'path', 'Route path', routeLimits.maxPathBytes)]
      }
      if (route.method && utf8Bytes(route.method) > routeLimits.maxMethodBytes) {
        return [limitIssue(route.id, 'method', 'Route method', routeLimits.maxMethodBytes)]
      }
    }
  }
  return []
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

function scalarText(value: unknown): string | null {
  if (typeof value === 'string') return value
  if (typeof value === 'number' || typeof value === 'boolean') return String(value)
  return null
}

function validateCompiledRouteLimits(
  route: RouteItemModel,
  value: Readonly<Record<string, unknown>>,
  limits: AccessConfigLimits,
): RouteValidationIssue[] {
  const routeLimits = limits.projectRoute
  const issues: RouteValidationIssue[] = []
  const stringFields = [
    ['path', 'Route path', routeLimits.maxPathBytes],
    ['method', 'Route method', routeLimits.maxMethodBytes],
    ['service', 'Route service', routeLimits.maxServiceBytes],
    ['cluster', 'Route cluster', routeLimits.maxClusterBytes],
    ['condition', 'Route condition', routeLimits.maxConditionBytes],
    ['rewrite', 'Route rewrite template', routeLimits.maxTemplateBytes],
    ['script', 'Route script', routeLimits.maxScriptBytes],
  ] as const
  for (const [field, resource, maximum] of stringFields) {
    const text = scalarText(value[field])
    if (text !== null && utf8Bytes(text) > maximum) {
      issues.push(limitIssue(route.id, field, resource, maximum))
    }
  }
  const service = scalarText(value.service)
  if (service !== null) {
    const slash = service.indexOf('/')
    if (
      slash > 0 &&
      slash + 1 < service.length &&
      utf8Bytes(service.slice(slash + 1)) > routeLimits.maxClusterBytes
    ) {
      issues.push(
        limitIssue(route.id, 'service', 'Route service cluster', routeLimits.maxClusterBytes),
      )
    }
  }

  for (const [field, resource, maxItems, maxBytes] of [
    ['addresses', 'Route address', routeLimits.maxAddressesPerRoute, routeLimits.maxAddressBytes],
    ['allows', 'Route CIDR', routeLimits.maxCidrsPerRoute, routeLimits.maxCidrBytes + 1],
  ] as const) {
    const entries = value[field]
    if (!Array.isArray(entries)) continue
    if (entries.length > maxItems) {
      issues.push(limitIssue(route.id, field, `${resource} count`, maxItems))
      continue
    }
    entries.forEach((entry, index) => {
      const text = scalarText(entry)
      if (text !== null && utf8Bytes(text) > maxBytes) {
        issues.push(limitIssue(route.id, `${field}.${index}`, resource, maxBytes))
      }
    })
  }

  for (const field of ['proxy_headers', 'response_headers', 'context'] as const) {
    const entries = value[field]
    if (typeof entries !== 'object' || entries === null || Array.isArray(entries)) continue
    const pairs = Object.entries(entries)
    if (pairs.length > routeLimits.maxHeaderEntries) {
      issues.push(
        limitIssue(route.id, field, 'Header or context entry count', routeLimits.maxHeaderEntries),
      )
      continue
    }
    pairs.forEach(([name, entry], index) => {
      if (utf8Bytes(name) > routeLimits.maxHeaderNameBytes) {
        issues.push(
          limitIssue(
            route.id,
            `${field}.${index}.name`,
            'Header or context name',
            routeLimits.maxHeaderNameBytes,
          ),
        )
      }
      const text = scalarText(entry)
      if (text !== null && utf8Bytes(text) > routeLimits.maxHeaderValueBytes) {
        issues.push(
          limitIssue(
            route.id,
            `${field}.${index}.value`,
            'Header or context value',
            routeLimits.maxHeaderValueBytes,
          ),
        )
      }
    })
  }

  const body = value.body
  if (typeof body === 'object' && body !== null && !Array.isArray(body)) {
    const bodyValue = body as Readonly<Record<string, unknown>>
    const content = scalarText(bodyValue.content)
    if (content !== null) {
      const maximum =
        bodyValue.type === 'TEXT'
          ? routeLimits.maxStaticResponseBodyBytes
          : bodyValue.type === 'BASE64'
            ? Math.ceil(routeLimits.maxStaticResponseBodyBytes / 3) * 4
            : routeLimits.maxTemplateBytes
      if (utf8Bytes(content) > maximum) {
        issues.push(limitIssue(route.id, 'body.content', 'Response body', maximum))
      }
    }
  }
  return issues
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

function validateResponseGzip(
  route: RouteItemModel,
  lineCounter: LineCounter,
  document: ReturnType<typeof parseDocument>,
  value: Readonly<Record<string, unknown>>,
  issues: RouteValidationIssue[],
): void {
  if (!('gzip' in value)) return

  const offset = findFieldOffset(document, 'gzip')
  const gzip = value.gzip
  const valid =
    typeof gzip === 'boolean' ||
    (typeof gzip === 'number' && Number.isInteger(gzip) && gzip >= 1 && gzip <= 9)
  if (!valid) {
    issues.push(
      issue(
        route,
        lineCounter,
        'INVALID_ROUTE_GZIP',
        'gzip must be true, false, or an integer compression level from 1 to 9',
        'gzip',
        offset,
      ),
    )
    return
  }
  if (value.type !== 'RESPONSE') {
    if (value.type === 'PROXY') {
      issues.push(
        issue(
          route,
          lineCounter,
          'ROUTE_GZIP_TYPE_CONFLICT',
          'gzip is only valid for RESPONSE routes',
          'gzip',
          offset,
        ),
      )
    }
    return
  }
  if (gzip === false) return

  const body = value.body
  if (typeof body !== 'object' || body === null || Array.isArray(body)) {
    issues.push(
      issue(
        route,
        lineCounter,
        'ROUTE_GZIP_BODY_CONFLICT',
        'Enabled gzip requires a non-empty TEXT or BASE64 response body',
        'gzip',
        offset,
      ),
    )
  } else {
    const bodyValue = body as Readonly<Record<string, unknown>>
    if (
      (bodyValue.type !== 'TEXT' && bodyValue.type !== 'BASE64') ||
      typeof bodyValue.content !== 'string' ||
      bodyValue.content.length === 0
    ) {
      issues.push(
        issue(
          route,
          lineCounter,
          'ROUTE_GZIP_BODY_CONFLICT',
          'Enabled gzip requires a non-empty TEXT or BASE64 response body',
          'gzip',
          offset,
        ),
      )
    }
  }

  const status =
    typeof value.status === 'number'
      ? value.status
      : typeof value.status === 'string' && /^[+-]?\d+$/u.test(value.status)
        ? Number(value.status)
        : null
  if (
    status !== null &&
    ((status >= 100 && status < 200) ||
      status === 204 ||
      status === 205 ||
      status === 206 ||
      status === 304)
  ) {
    issues.push(
      issue(
        route,
        lineCounter,
        'ROUTE_GZIP_STATUS_CONFLICT',
        'gzip is not supported for informational, 204, 205, 206, or 304 responses',
        'gzip',
        offset,
      ),
    )
  }

  const responseHeaders = value.response_headers
  if (
    typeof responseHeaders === 'object' &&
    responseHeaders !== null &&
    !Array.isArray(responseHeaders) &&
    Object.keys(responseHeaders).some((name) => name.toLowerCase() === 'content-encoding')
  ) {
    issues.push(
      issue(
        route,
        lineCounter,
        'ROUTE_GZIP_CONTENT_ENCODING_CONFLICT',
        'Remove response_headers.Content-Encoding when gzip is enabled',
        'gzip',
        offset,
      ),
    )
  }
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
  validateResponseGzip(route, lineCounter, document, value, issues)
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
  limits: AccessConfigLimits = fallbackAccessConfigLimits,
): ProjectRoutesCompileResult {
  const limitIssues = validateModelLimits(domain, model, limits)
  if (limitIssues.length > 0) return { compiled: null, issues: limitIssues }
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
      let compiledRoute: Readonly<Record<string, unknown>> | null = null
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
        compiledRoute = allows.length > 0 ? { ...parsed.value, allows } : parsed.value
      } else {
        compiledRoute = parsed.value
      }
      if (compiledRoute) {
        const compiledLimitIssues = validateCompiledRouteLimits(route, compiledRoute, limits)
        issues.push(...compiledLimitIssues)
        if (compiledLimitIssues.length === 0) routes.push(compiledRoute)
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
  if (payload.byteLength > limits.projectRoute.maxPayloadBytes) {
    return {
      compiled: null,
      issues: [
        limitIssue(
          model.routes.at(-1)?.id ?? networkPolicyRouteId,
          'payload',
          'Compiled project route payload',
          limits.projectRoute.maxPayloadBytes,
        ),
      ],
    }
  }
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
