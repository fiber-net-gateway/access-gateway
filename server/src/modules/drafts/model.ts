export type DraftState = 'editing' | 'validating' | 'ready'

export interface ProjectRouteModel {
  schemaVersion: 1
  kind: 'project_route'
  hosts: readonly unknown[]
  routes: readonly unknown[]
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
  model: ProjectRouteModel
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

export function isProjectRouteModel(value: unknown): value is ProjectRouteModel {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false
  const model = value as Record<string, unknown>
  return (
    model.schemaVersion === 1 &&
    model.kind === 'project_route' &&
    Array.isArray(model.hosts) &&
    Array.isArray(model.routes)
  )
}
