import { createHash } from 'node:crypto'

import { stringify } from 'yaml'

import type { AccessConfigLimits } from '../../integrations/native-validator/model.js'
import {
  fallbackAccessConfigLimits,
  utf8Bytes,
} from '../../integrations/native-validator/limits.js'
import { bufferToPublicId, isPublicId } from '../../shared/ids.js'
import { canonicalJson } from '../../shared/json.js'
import { isCanonicalExactHost, normalizeExactHost } from '../../shared/hosts.js'

export type DraftState = 'editing' | 'validating' | 'ready'

export interface YamlRouteItemModel {
  id: string
  format: 'yaml'
  source: string
}

export interface JavaScriptRouteItemModel {
  id: string
  format: 'js'
  source: string
  path: string
  method?: string
  gzip?: boolean | number
}

export type RouteItemModel = YamlRouteItemModel | JavaScriptRouteItemModel

export type HttpsRedirect = 'off' | '301' | '302' | '307' | '308'

export const PROJECT_ROUTES_SCHEMA_VERSION = 6

export interface ProjectNetworkPolicy {
  source: 'route' | 'project'
  httpsRedirect: HttpsRedirect
  allowedCidrs: readonly string[]
  deniedCidrs: readonly string[]
}

export interface ProjectRoutesModel {
  schemaVersion: 6
  kind: 'project_routes_yaml'
  /** Exact host aliases in addition to the immutable project domain. */
  hostAliases: readonly string[]
  networkPolicy: ProjectNetworkPolicy
  routes: readonly RouteItemModel[]
}

export interface DraftView {
  id: string
  projectId: string
  state: DraftState
  title: string
  currentRevision: number
  lockVersion: string
  createdAt: string
  updatedAt: string
}

export interface DraftRevisionView {
  id: string
  draftId: string
  revision: number
  model: ProjectRoutesModel
  modelSha256: string
  validationState: 'not_run' | 'pending' | 'valid' | 'invalid'
  changeSummary: string
  createdAt: string
}

export interface CreateDraftRevisionInput {
  lockVersion: string
  changeSummary: string
  model: unknown
}

export function isProjectRoutesModel(
  value: unknown,
  limits: AccessConfigLimits = fallbackAccessConfigLimits,
): value is ProjectRoutesModel {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  if (
    model.schemaVersion !== PROJECT_ROUTES_SCHEMA_VERSION ||
    model.kind !== 'project_routes_yaml' ||
    !Array.isArray(model.hostAliases) ||
    model.hostAliases.length > Math.max(limits.projectRoute.maxHosts - 1, 0) ||
    typeof model.networkPolicy !== 'object' ||
    model.networkPolicy === null ||
    Array.isArray(model.networkPolicy) ||
    !Array.isArray(model.routes) ||
    model.routes.length > limits.projectRoute.maxRoutes
  ) {
    return false
  }
  const hostAliases = model.hostAliases
  const hostAliasSet = new Set<string>()
  if (
    !hostAliases.every(
      (alias) =>
        typeof alias === 'string' &&
        isCanonicalExactHost(alias) &&
        utf8Bytes(alias) <= limits.projectRoute.maxHostPatternBytes &&
        !hostAliasSet.has(alias) &&
        hostAliasSet.add(alias),
    )
  ) {
    return false
  }
  const networkPolicy = model.networkPolicy as Record<string, unknown>
  if (
    (networkPolicy.source !== 'route' && networkPolicy.source !== 'project') ||
    !['off', '301', '302', '307', '308'].includes(String(networkPolicy.httpsRedirect)) ||
    !Array.isArray(networkPolicy.allowedCidrs) ||
    !Array.isArray(networkPolicy.deniedCidrs) ||
    networkPolicy.allowedCidrs.length + networkPolicy.deniedCidrs.length >
      limits.projectRoute.maxCidrsPerRoute ||
    ![...networkPolicy.allowedCidrs, ...networkPolicy.deniedCidrs].every(
      (cidr) =>
        typeof cidr === 'string' &&
        cidr.length > 0 &&
        utf8Bytes(cidr) <= limits.projectRoute.maxCidrBytes,
    )
  ) {
    return false
  }
  const ids = new Set<string>()
  let totalSourceBytes = 0
  for (const value of model.routes) {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
    const route = value as Record<string, unknown>
    if (
      !isPublicId(String(route.id)) ||
      typeof route.source !== 'string' ||
      (route.format !== 'yaml' && route.format !== 'js')
    ) {
      return false
    }
    if (
      route.format === 'js' &&
      (typeof route.path !== 'string' ||
        route.path.length < 1 ||
        utf8Bytes(route.path) > limits.projectRoute.maxPathBytes ||
        (route.method !== undefined &&
          (typeof route.method !== 'string' ||
            route.method.length < 1 ||
            utf8Bytes(route.method) > limits.projectRoute.maxMethodBytes)))
    ) {
      return false
    }
    if (
      route.format === 'js' &&
      route.gzip !== undefined &&
      !(
        typeof route.gzip === 'boolean' ||
        (typeof route.gzip === 'number' &&
          Number.isInteger(route.gzip) &&
          route.gzip >= 1 &&
          route.gzip <= 9)
      )
    ) {
      return false
    }
    const sourceBytes = utf8Bytes(route.source)
    const sourceLimit =
      route.format === 'js'
        ? limits.projectRoute.maxScriptBytes
        : limits.projectRoute.maxPayloadBytes
    if (sourceBytes > sourceLimit || ids.has(route.id as string)) return false
    totalSourceBytes += sourceBytes
    if (totalSourceBytes > limits.projectRoute.maxPayloadBytes) return false
    ids.add(route.id as string)
  }
  return true
}

function isProjectRoutesModelV5(value: unknown, limits: AccessConfigLimits): boolean {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  if (model.schemaVersion !== 5 || 'hostAliases' in model) return false
  return isProjectRoutesModel(
    { ...model, schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION, hostAliases: [] },
    limits,
  )
}

interface LegacyYamlRouteItemModel {
  id: string
  source: string
}

interface YamlRoutesV4Model {
  schemaVersion: 4
  kind: 'project_routes_yaml'
  networkPolicy: ProjectNetworkPolicy
  routes: readonly LegacyYamlRouteItemModel[]
}

function isYamlRoutesV4Model(value: unknown): value is YamlRoutesV4Model {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  if (
    model.schemaVersion !== 4 ||
    model.kind !== 'project_routes_yaml' ||
    !Array.isArray(model.routes) ||
    model.routes.length > fallbackAccessConfigLimits.projectRoute.maxRoutes
  ) {
    return false
  }
  return isProjectRoutesModel({
    ...model,
    schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION,
    hostAliases: [],
    routes: model.routes.map((route) =>
      typeof route === 'object' && route !== null && !Array.isArray(route)
        ? { ...route, format: 'yaml' }
        : route,
    ),
  })
}

interface YamlRoutesV3Model {
  schemaVersion: 3
  kind: 'project_routes_yaml'
  networkPolicy: Omit<ProjectNetworkPolicy, 'httpsRedirect'>
  routes: readonly LegacyYamlRouteItemModel[]
}

function isYamlRoutesV3Model(value: unknown): value is YamlRoutesV3Model {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  if (
    model.schemaVersion !== 3 ||
    model.kind !== 'project_routes_yaml' ||
    typeof model.networkPolicy !== 'object' ||
    model.networkPolicy === null ||
    Array.isArray(model.networkPolicy) ||
    !Array.isArray(model.routes) ||
    model.routes.length > fallbackAccessConfigLimits.projectRoute.maxRoutes
  ) {
    return false
  }
  const networkPolicy = model.networkPolicy as Record<string, unknown>
  return isProjectRoutesModel({
    ...model,
    schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION,
    hostAliases: [],
    networkPolicy: { ...networkPolicy, httpsRedirect: 'off' },
    routes: model.routes.map((route) =>
      typeof route === 'object' && route !== null && !Array.isArray(route)
        ? { ...route, format: 'yaml' }
        : route,
    ),
  })
}

interface LegacyProjectRouteModel {
  schemaVersion: 1
  kind: 'project_route'
  hosts: readonly unknown[]
  routes: readonly unknown[]
}

interface YamlRoutesV2Model {
  schemaVersion: 2
  kind: 'project_routes_yaml'
  routes: readonly LegacyYamlRouteItemModel[]
}

function isYamlRoutesV2Model(value: unknown): value is YamlRoutesV2Model {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  if (
    model.schemaVersion !== 2 ||
    model.kind !== 'project_routes_yaml' ||
    !Array.isArray(model.routes) ||
    model.routes.length > fallbackAccessConfigLimits.projectRoute.maxRoutes
  ) {
    return false
  }
  const ids = new Set<string>()
  let totalSourceBytes = 0
  for (const value of model.routes) {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
    const route = value as Record<string, unknown>
    if (!isPublicId(String(route.id)) || typeof route.source !== 'string') return false
    if (
      utf8Bytes(route.source) > fallbackAccessConfigLimits.projectRoute.maxPayloadBytes ||
      ids.has(route.id as string)
    ) {
      return false
    }
    totalSourceBytes += utf8Bytes(route.source)
    if (totalSourceBytes > fallbackAccessConfigLimits.projectRoute.maxPayloadBytes) return false
    ids.add(route.id as string)
  }
  return true
}

function isLegacyProjectRouteModel(value: unknown): value is LegacyProjectRouteModel {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  return (
    model.schemaVersion === 1 &&
    model.kind === 'project_route' &&
    Array.isArray(model.hosts) &&
    Array.isArray(model.routes) &&
    model.routes.length <= fallbackAccessConfigLimits.projectRoute.maxRoutes
  )
}

function legacyRouteId(route: unknown, index: number): string {
  const bytes = createHash('sha256')
    .update(`legacy-route:${index}:`)
    .update(canonicalJson(route))
    .digest()
    .subarray(0, 16)
  bytes[6] = (bytes[6]! & 0x0f) | 0x50
  bytes[8] = (bytes[8]! & 0x3f) | 0x80
  return bufferToPublicId(bytes)
}

export function normalizeStoredProjectRoutesModel(
  value: unknown,
  limits: AccessConfigLimits = fallbackAccessConfigLimits,
  primaryDomain?: string,
): ProjectRoutesModel | null {
  const primary = primaryDomain ? normalizeExactHost(primaryDomain) : null
  if (isProjectRoutesModel(value, limits)) return value
  if (isProjectRoutesModelV5(value, limits)) {
    const legacy = value as Record<string, unknown>
    return {
      ...(legacy as unknown as Omit<ProjectRoutesModel, 'schemaVersion' | 'hostAliases'>),
      schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION,
      hostAliases: [],
    }
  }
  if (isYamlRoutesV4Model(value)) {
    return {
      ...value,
      schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION,
      hostAliases: [],
      routes: value.routes.map((route) => ({ ...route, format: 'yaml' })),
    }
  }
  if (isYamlRoutesV3Model(value)) {
    return {
      ...value,
      schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION,
      hostAliases: [],
      networkPolicy: { ...value.networkPolicy, httpsRedirect: 'off' },
      routes: value.routes.map((route) => ({ ...route, format: 'yaml' })),
    }
  }
  if (isYamlRoutesV2Model(value)) {
    return {
      schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION,
      kind: 'project_routes_yaml',
      hostAliases: [],
      networkPolicy: {
        source: 'route',
        httpsRedirect: 'off',
        allowedCidrs: [],
        deniedCidrs: [],
      },
      routes: value.routes.map((route) => ({ ...route, format: 'yaml' })),
    }
  }
  if (!isLegacyProjectRouteModel(value)) return null
  const routes: YamlRouteItemModel[] = []
  let totalSourceBytes = 0
  for (const [index, route] of value.routes.entries()) {
    const source = stringify(route, { lineWidth: 0 }).trimEnd()
    const sourceBytes = utf8Bytes(source)
    totalSourceBytes += sourceBytes
    if (
      sourceBytes > fallbackAccessConfigLimits.projectRoute.maxPayloadBytes ||
      totalSourceBytes > fallbackAccessConfigLimits.projectRoute.maxPayloadBytes
    ) {
      return null
    }
    routes.push({
      id: legacyRouteId(route, index),
      format: 'yaml',
      source,
    })
  }
  const hostAliases: string[] = []
  for (const host of value.hosts) {
    if (typeof host !== 'object' || host === null || Array.isArray(host)) return null
    const pattern = (host as Record<string, unknown>).pattern
    if (typeof pattern !== 'string') return null
    const normalized = normalizeExactHost(pattern)
    if (!normalized) return null
    if (normalized === primary) continue
    if (!hostAliases.includes(normalized)) hostAliases.push(normalized)
  }
  if (hostAliases.length > Math.max(limits.projectRoute.maxHosts - 1, 0)) return null
  if (hostAliases.some((alias) => utf8Bytes(alias) > limits.projectRoute.maxHostPatternBytes)) {
    return null
  }
  return {
    schemaVersion: PROJECT_ROUTES_SCHEMA_VERSION,
    kind: 'project_routes_yaml',
    hostAliases,
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
    routes,
  }
}

/** Normalize a request model before it is compiled and persisted. */
export function normalizeProjectRoutesModelInput(
  value: unknown,
  limits: AccessConfigLimits = fallbackAccessConfigLimits,
): ProjectRoutesModel | null {
  if (typeof value === 'object' && value !== null && !Array.isArray(value)) {
    const candidate = value as Record<string, unknown>
    if (candidate.schemaVersion === PROJECT_ROUTES_SCHEMA_VERSION) {
      if (!Array.isArray(candidate.hostAliases)) return null
      const aliases = candidate.hostAliases.map((alias) =>
        typeof alias === 'string' ? normalizeExactHost(alias) : null,
      )
      if (aliases.some((alias): alias is null => alias === null)) return null
      return normalizeStoredProjectRoutesModel({ ...candidate, hostAliases: aliases }, limits)
    }
    if (candidate.schemaVersion === 5) {
      if ('hostAliases' in candidate || !isProjectRoutesModelV5(candidate, limits)) return null
      return normalizeStoredProjectRoutesModel(candidate, limits)
    }
  }
  return null
}
