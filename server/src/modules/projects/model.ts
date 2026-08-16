import type { ActivationStatus } from '../activation/model.js'

export interface ProjectView {
  id: string
  domain: string
  status: 'active' | 'decommissioning' | 'archived'
  lockVersion: string
  draft: {
    id: string
    state: string
    revision: number
    lockVersion: string
  } | null
  publishedVersion: number | null
  activationStatus: ActivationStatus
  createdAt: string
  updatedAt: string
}

export interface CreateProjectInput {
  domain: string
}

export interface ProjectListResult {
  items: readonly ProjectView[]
}
