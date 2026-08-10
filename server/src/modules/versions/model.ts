import type { ProjectRoutesModel } from '../drafts/model.js'
import type { ProjectRoutesValidationView } from '../drafts/validation.js'
import type { ReleaseStatus } from '../releases/state.js'

export type ConfigurationVersionValidationState = 'not_run' | 'pending' | 'valid' | 'invalid'
export type ConfigurationVersionPublicationState = ReleaseStatus | 'never'

export interface ConfigurationVersionSummary {
  id: string
  projectId: string
  number: number
  relation: 'current' | 'historical'
  baseVersionId: string | null
  restoredFromVersionId: string | null
  changeSummary: string
  routeCount: number
  modelSha256: string
  validationState: ConfigurationVersionValidationState
  publicationStatus: ConfigurationVersionPublicationState
  createdBy: {
    id: string
    displayName: string
  }
  createdAt: string
}

export interface ConfigurationVersionDetail extends ConfigurationVersionSummary {
  model: ProjectRoutesModel
}

export interface ConfigurationVersionListResult {
  items: readonly ConfigurationVersionSummary[]
  nextCursor: string | null
  currentVersionId: string | null
  lockVersion: string
}

export interface SaveConfigurationVersionInput {
  lockVersion: string
  baseVersionId: string | null
  changeSummary: string
  forceSameContent: boolean
  idempotencyKey: string
  model: unknown
}

export interface RestoreConfigurationVersionInput {
  lockVersion: string
  baseVersionId: string
  changeSummary: string
  forceSameContent: boolean
  idempotencyKey: string
}

export interface SavedConfigurationVersion {
  version: ConfigurationVersionDetail
  lockVersion: string
}

export interface ValidatedConfigurationVersion {
  version: ConfigurationVersionSummary
  validation: ProjectRoutesValidationView
}
