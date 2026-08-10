import type { ProjectRouteModel } from '../api/types'

export function initialRouteModel(domain: string): ProjectRouteModel {
  return {
    schemaVersion: 1,
    kind: 'project_route',
    hosts: [{ pattern: domain }],
    routes: [],
  }
}

export function parseRouteModel(document: string): ProjectRouteModel {
  const parsed: unknown = JSON.parse(document)
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new Error('路由模型必须是 JSON object')
  }
  const model = parsed as Record<string, unknown>
  if (
    model.schemaVersion !== 1 ||
    model.kind !== 'project_route' ||
    !Array.isArray(model.hosts) ||
    !Array.isArray(model.routes)
  ) {
    throw new Error('路由模型必须包含 schemaVersion=1、kind=project_route、hosts 和 routes')
  }
  return parsed as ProjectRouteModel
}
