export interface ProjectView {
  id: string
  domain: string
  status: 'active' | 'archived'
  lockVersion: string
  draft: {
    id: string
    state: string
    revision: number
    lockVersion: string
  } | null
  publishedVersion: number | null
  activationStatus: 'unknown'
  createdAt: string
  updatedAt: string
}

export interface CreateProjectInput {
  domain: string
}

export interface ProjectListResult {
  items: readonly ProjectView[]
}
