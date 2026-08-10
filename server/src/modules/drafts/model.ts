import { createHash } from 'node:crypto'

import { stringify } from 'yaml'

import { bufferToPublicId, isPublicId } from '../../shared/ids.js'
import { canonicalJson } from '../../shared/json.js'

export type DraftState = 'editing' | 'validating' | 'ready'

export interface RouteItemModel {
  id: string
  source: string
}

export interface ProjectRoutesModel {
  schemaVersion: 2
  kind: 'project_routes_yaml'
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

export function isProjectRoutesModel(value: unknown): value is ProjectRoutesModel {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  if (
    model.schemaVersion !== 2 ||
    model.kind !== 'project_routes_yaml' ||
    !Array.isArray(model.routes) ||
    model.routes.length > 5_000
  ) {
    return false
  }
  const ids = new Set<string>()
  let totalSourceBytes = 0
  for (const value of model.routes) {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
    const route = value as Record<string, unknown>
    if (!isPublicId(String(route.id)) || typeof route.source !== 'string') return false
    if (route.source.length > 1_048_576 || ids.has(route.id as string)) return false
    totalSourceBytes += Buffer.byteLength(route.source, 'utf8')
    if (totalSourceBytes > 4_194_304) return false
    ids.add(route.id as string)
  }
  return true
}

interface LegacyProjectRouteModel {
  schemaVersion: 1
  kind: 'project_route'
  hosts: readonly unknown[]
  routes: readonly unknown[]
}

function isLegacyProjectRouteModel(value: unknown): value is LegacyProjectRouteModel {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  return (
    model.schemaVersion === 1 &&
    model.kind === 'project_route' &&
    Array.isArray(model.hosts) &&
    Array.isArray(model.routes)
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

export function normalizeStoredProjectRoutesModel(value: unknown): ProjectRoutesModel | null {
  if (isProjectRoutesModel(value)) return value
  if (!isLegacyProjectRouteModel(value)) return null
  return {
    schemaVersion: 2,
    kind: 'project_routes_yaml',
    routes: value.routes.map((route, index) => ({
      id: legacyRouteId(route, index),
      source: stringify(route, { lineWidth: 0 }).trimEnd(),
    })),
  }
}
