import { isAlias, isMap, isScalar, LineCounter, parseDocument, visit } from 'yaml'

import type { ProjectRoutesModel, RouteItemModel, RouteValidationIssue } from '../api/types'

export type RouteTemplate = 'RESPONSE' | 'PROXY' | 'JS'

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
  'upstream_tls',
])

const responseTemplate = `path: /healthz
type: RESPONSE
status: 200
body:
  type: TEXT
  content: ok
gzip: true
response_headers:
  Content-Type: text/plain; charset=utf-8
`

const proxyTemplate = `path: /api/*
type: PROXY
service: example-service/stable
timeout: 30s
rewrite: /internal
`

const javaScriptTemplate = `return {
  path: $req.path,
  method: $req.method,
}
`

export interface RouteSourceAnalysis {
  path: string | null
  method: string | null
  type: 'RESPONSE' | 'PROXY' | 'SCRIPT' | null
  condition: string | null
  hasUpstreamTls: boolean
  issues: readonly RouteValidationIssue[]
}

const analysisCache = new WeakMap<RouteItemModel, RouteSourceAnalysis>()

export function initialRouteModel(): ProjectRoutesModel {
  return {
    schemaVersion: 5,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
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
  if (template === 'JS') {
    return {
      id: createRouteId(),
      format: 'js',
      path: '/script/:id',
      source: javaScriptTemplate,
    }
  }
  return {
    id: createRouteId(),
    format: 'yaml',
    source: template === 'RESPONSE' ? responseTemplate : proxyTemplate,
  }
}

export function duplicateRouteItem(route: RouteItemModel): RouteItemModel {
  return { ...route, id: createRouteId() }
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

function isJsonSafe(value: unknown): boolean {
  if (value === null || typeof value === 'string' || typeof value === 'boolean') return true
  if (typeof value === 'number') return Number.isFinite(value) && Number.isSafeInteger(value)
  if (Array.isArray(value)) return value.every(isJsonSafe)
  if (typeof value !== 'object') return false
  return Object.entries(value).every(([key, item]) => typeof key === 'string' && isJsonSafe(item))
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
      sourceIssue(
        route.id,
        lineCounter,
        'INVALID_ROUTE_FIELD_TYPE',
        `${field} 必须是${expected}`,
        field,
        fieldOffset(document, field),
      ),
    )
  }

  for (const field of ['proxy_headers', 'response_headers', 'context']) {
    if (field in value && !isScalarMap(value[field])) addTypeIssue(field, 'mapping 或 null')
  }
  for (const field of ['addresses', 'allows']) {
    if (field in value && !isScalarList(value[field])) addTypeIssue(field, 'sequence 或 null')
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
    if (field in value && !isScalarValue(value[field])) addTypeIssue(field, 'scalar 或 null')
  }
  if ('body' in value && !isScalarMap(value.body)) addTypeIssue('body', 'mapping 或 null')
  validateUpstreamTls(route, lineCounter, document, value, issues)
}

const upstreamTlsFields = new Set([
  'generation',
  'verification',
  'ca_pem',
  'server_name',
  'verify_name',
  'client_identity_ref',
])
const upstreamTlsVerificationModes = new Set([
  'INHERIT',
  'LEGACY_INSECURE',
  'SYSTEM_CA',
  'CUSTOM_CA',
])

function validTlsDnsName(value: string): boolean {
  if (
    value.length < 1 ||
    value.length > 253 ||
    value.startsWith('.') ||
    value.endsWith('.') ||
    validTlsIp(value)
  ) {
    return false
  }
  return value
    .split('.')
    .every(
      (label) =>
        label.length >= 1 &&
        label.length <= 63 &&
        /^[A-Za-z0-9-]+$/u.test(label) &&
        !label.includes('*'),
    )
}

function validPublicId(value: string): boolean {
  return /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/iu.test(value)
}

function validTlsIp(value: string): boolean {
  const ipv4 = value.split('.')
  if (
    ipv4.length === 4 &&
    ipv4.every((part) => /^\d{1,3}$/u.test(part) && Number(part) >= 0 && Number(part) <= 255)
  ) {
    return true
  }
  if (!value.includes(':') || !/^[0-9A-Fa-f:.]+$/u.test(value)) return false
  try {
    return new URL(`http://[${value}]/`).hostname.startsWith('[')
  } catch {
    return false
  }
}

function validateUpstreamTls(
  route: RouteItemModel,
  lineCounter: LineCounter,
  document: ReturnType<typeof parseDocument>,
  value: Readonly<Record<string, unknown>>,
  issues: RouteValidationIssue[],
): void {
  if (!('upstream_tls' in value) || value.upstream_tls === null) return
  const offset = fieldOffset(document, 'upstream_tls')
  const add = (code: string, message: string, path = 'upstream_tls'): void => {
    issues.push(sourceIssue(route.id, lineCounter, code, message, path, offset))
  }
  if (
    typeof value.upstream_tls !== 'object' ||
    value.upstream_tls === null ||
    Array.isArray(value.upstream_tls)
  ) {
    add('INVALID_UPSTREAM_TLS_PROFILE', 'upstream_tls 必须是 mapping 或 null')
    return
  }
  const profile = value.upstream_tls as Readonly<Record<string, unknown>>
  for (const field of Object.keys(profile)) {
    if (!upstreamTlsFields.has(field)) {
      add('UNKNOWN_UPSTREAM_TLS_FIELD', `未知 upstream_tls 字段：${field}`, `upstream_tls.${field}`)
    }
  }
  if (
    typeof profile.generation !== 'number' ||
    !Number.isSafeInteger(profile.generation) ||
    profile.generation <= 0
  ) {
    add('INVALID_UPSTREAM_TLS_GENERATION', 'generation 必须是正安全整数', 'upstream_tls.generation')
  }
  if (
    profile.verification !== undefined &&
    (typeof profile.verification !== 'string' ||
      !upstreamTlsVerificationModes.has(profile.verification))
  ) {
    add(
      'INVALID_UPSTREAM_TLS_VERIFICATION',
      'verification 必须是 INHERIT、LEGACY_INSECURE、SYSTEM_CA 或 CUSTOM_CA',
      'upstream_tls.verification',
    )
  }
  const verification = profile.verification ?? 'INHERIT'
  if (
    profile.ca_pem !== undefined &&
    profile.ca_pem !== null &&
    typeof profile.ca_pem !== 'string'
  ) {
    add('INVALID_UPSTREAM_TLS_CA', 'ca_pem 必须是字符串或 null', 'upstream_tls.ca_pem')
  } else if (
    verification === 'CUSTOM_CA' &&
    (typeof profile.ca_pem !== 'string' || profile.ca_pem.length === 0)
  ) {
    add('INVALID_UPSTREAM_TLS_CA', 'CUSTOM_CA 必须提供非空 ca_pem', 'upstream_tls.ca_pem')
  } else if (verification !== 'CUSTOM_CA' && typeof profile.ca_pem === 'string') {
    add('UPSTREAM_TLS_CA_CONFLICT', '只有 CUSTOM_CA 可以配置 ca_pem', 'upstream_tls.ca_pem')
  }
  if (
    profile.server_name !== undefined &&
    profile.server_name !== null &&
    (typeof profile.server_name !== 'string' || !validTlsDnsName(profile.server_name))
  ) {
    add(
      'INVALID_UPSTREAM_TLS_SERVER_NAME',
      'server_name 必须是非空 ASCII DNS 名称',
      'upstream_tls.server_name',
    )
  }
  if (
    profile.verify_name !== undefined &&
    profile.verify_name !== null &&
    (typeof profile.verify_name !== 'string' ||
      (!validTlsIp(profile.verify_name) && !validTlsDnsName(profile.verify_name)))
  ) {
    add(
      'INVALID_UPSTREAM_TLS_VERIFY_NAME',
      'verify_name 必须是非空 ASCII DNS 名称或 IP 地址',
      'upstream_tls.verify_name',
    )
  }
  if (verification === 'LEGACY_INSECURE' && typeof profile.verify_name === 'string') {
    add(
      'UPSTREAM_TLS_VERIFY_NAME_CONFLICT',
      'LEGACY_INSECURE 不能配置 verify_name',
      'upstream_tls.verify_name',
    )
  }
  if (
    profile.client_identity_ref !== undefined &&
    profile.client_identity_ref !== null &&
    (typeof profile.client_identity_ref !== 'string' || !validPublicId(profile.client_identity_ref))
  ) {
    add(
      'INVALID_UPSTREAM_TLS_CLIENT_IDENTITY_REF',
      'client_identity_ref 必须是有效 UUID 或 null',
      'upstream_tls.client_identity_ref',
    )
  }
  if (value.type !== 'PROXY') {
    add('UPSTREAM_TLS_ROUTE_TYPE_CONFLICT', 'upstream_tls 只能用于 PROXY Route')
  }
}

function validateResponseGzip(
  route: RouteItemModel,
  lineCounter: LineCounter,
  document: ReturnType<typeof parseDocument>,
  value: Readonly<Record<string, unknown>>,
  issues: RouteValidationIssue[],
): void {
  if (!('gzip' in value)) return

  const offset = fieldOffset(document, 'gzip')
  const gzip = value.gzip
  const valid =
    typeof gzip === 'boolean' ||
    (typeof gzip === 'number' && Number.isInteger(gzip) && gzip >= 1 && gzip <= 9)
  if (!valid) {
    issues.push(
      sourceIssue(
        route.id,
        lineCounter,
        'INVALID_ROUTE_GZIP',
        'gzip 必须是 true、false，或 1-9 的整数压缩级别',
        'gzip',
        offset,
      ),
    )
    return
  }
  if (value.type !== 'RESPONSE') {
    if (value.type === 'PROXY') {
      issues.push(
        sourceIssue(
          route.id,
          lineCounter,
          'ROUTE_GZIP_TYPE_CONFLICT',
          'gzip 只能用于 RESPONSE Route',
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
      sourceIssue(
        route.id,
        lineCounter,
        'ROUTE_GZIP_BODY_CONFLICT',
        '启用 gzip 时必须配置非空的 TEXT 或 BASE64 response body',
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
        sourceIssue(
          route.id,
          lineCounter,
          'ROUTE_GZIP_BODY_CONFLICT',
          '启用 gzip 时必须配置非空的 TEXT 或 BASE64 response body',
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
      sourceIssue(
        route.id,
        lineCounter,
        'ROUTE_GZIP_STATUS_CONFLICT',
        'gzip 不支持 1xx、204、205、206 或 304 response',
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
      sourceIssue(
        route.id,
        lineCounter,
        'ROUTE_GZIP_CONTENT_ENCODING_CONFLICT',
        '启用 gzip 时不能同时配置 response_headers.Content-Encoding',
        'gzip',
        offset,
      ),
    )
  }
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
  if (route.format === 'js') {
    const issues: RouteValidationIssue[] = []
    if (route.path.length < 1 || route.path.length > 2048) {
      issues.push({
        routeId: route.id,
        path: 'path',
        line: 1,
        column: 1,
        code: 'INVALID_ROUTE_PATH',
        message: 'JS Route path 必须包含 1-2048 个字符',
      })
    }
    if (
      route.method !== undefined &&
      (route.method.length > 64 || !/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/u.test(route.method))
    ) {
      issues.push({
        routeId: route.id,
        path: 'method',
        line: 1,
        column: 1,
        code: 'INVALID_ROUTE_METHOD',
        message: 'method 必须是 1-64 字节的 HTTP token；留空表示所有 method',
      })
    }
    if (route.source.trim().length === 0) {
      issues.push({
        routeId: route.id,
        path: 'source',
        line: 1,
        column: 1,
        code: 'EMPTY_ROUTE_SCRIPT',
        message: 'JS Route 脚本不能为空',
      })
    }
    return {
      path: route.path || null,
      method: route.method ?? null,
      type: 'SCRIPT',
      condition: null,
      hasUpstreamTls: false,
      issues,
    }
  }
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
  for (const warning of document.warnings) {
    issues.push({
      routeId: route.id,
      path: '',
      ...position(lineCounter, warning.pos[0]),
      code: warning.code,
      message: warning.message,
    })
  }
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
    return {
      path: null,
      method: null,
      type: null,
      condition: null,
      hasUpstreamTls: false,
      issues,
    }
  }
  if (issues.length > 0) {
    return {
      path: null,
      method: null,
      type: null,
      condition: null,
      hasUpstreamTls: false,
      issues,
    }
  }
  const value: unknown = document.toJS({ maxAliasCount: 0 })
  if (typeof value !== 'object' || value === null || Array.isArray(value) || !isJsonSafe(value)) {
    if (!isJsonSafe(value)) {
      issues.push({
        routeId: route.id,
        path: '',
        line: 1,
        column: 1,
        code: 'ROUTE_VALUE_NOT_JSON_SAFE',
        message: 'Route 值只能使用 JSON 安全的字符串、布尔值、数组、对象和安全整数',
      })
    }
    return {
      path: null,
      method: null,
      type: null,
      condition: null,
      hasUpstreamTls: false,
      issues,
    }
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
  if (
    routeValue.method !== undefined &&
    routeValue.method !== null &&
    (typeof routeValue.method !== 'string' ||
      routeValue.method.length > 64 ||
      !/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/u.test(routeValue.method))
  ) {
    issues.push(
      sourceIssue(
        route.id,
        lineCounter,
        'INVALID_ROUTE_METHOD',
        'method 必须是 1-64 字节的 HTTP token；不配置表示所有 method',
        'method',
        fieldOffset(document, 'method'),
      ),
    )
  }
  validateRouteFieldShapes(route, lineCounter, document, routeValue, issues)
  validateResponseGzip(route, lineCounter, document, routeValue, issues)
  return {
    path: typeof routeValue.path === 'string' ? routeValue.path : null,
    method: typeof routeValue.method === 'string' ? routeValue.method : null,
    type,
    condition: typeof routeValue.condition === 'string' ? routeValue.condition : null,
    hasUpstreamTls: routeValue.upstream_tls !== undefined && routeValue.upstream_tls !== null,
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
