import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import { bufferToPublicId } from '../../shared/ids.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import type {
  CreateDraftRevisionInput,
  DraftRevisionView,
  DraftView,
  ProjectRouteModel,
} from './model.js'
import { isProjectRouteModel } from './model.js'
import { DraftRepository } from './repository.js'

export interface DraftService {
  get(actor: Actor, projectId: string): Promise<DraftView>
  getOrCreate(actor: Actor, projectId: string, requestId: string): Promise<DraftView>
  createRevision(
    actor: Actor,
    draftId: string,
    input: CreateDraftRevisionInput,
    requestId: string,
  ): Promise<DraftRevisionView>
  getRevision(actor: Actor, draftId: string, revisionId: string): Promise<DraftRevisionView>
}

function parseProjectRouteModel(value: unknown): ProjectRouteModel {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw badRequest('INVALID_DRAFT_MODEL', 'Draft model must be an object')
  }
  if (!isProjectRouteModel(value)) {
    throw badRequest(
      'INVALID_DRAFT_MODEL',
      'Draft model does not match project_route schema version 1',
      [{ path: 'model', code: 'INVALID_SCHEMA', message: 'Invalid project route model' }],
    )
  }
  return value
}

function parseLockVersion(value: string): string {
  if (!/^(0|[1-9][0-9]*)$/u.test(value)) {
    throw badRequest('INVALID_LOCK_VERSION', 'lockVersion must be an unsigned integer string')
  }
  return value
}

export class DefaultDraftService implements DraftService {
  readonly #drafts: DraftRepository
  readonly #projects: ProjectRepository
  readonly #environments: EnvironmentRepository

  constructor(
    drafts: DraftRepository,
    projects: ProjectRepository,
    environments: EnvironmentRepository,
  ) {
    this.#drafts = drafts
    this.#projects = projects
    this.#environments = environments
  }

  async getOrCreate(actor: Actor, projectId: string, requestId: string): Promise<DraftView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) {
      throw notFound('Project')
    }
    await this.requireEditor(actor, bufferToPublicId(project.environment_public_id))
    return this.#drafts.getOrCreate(actor, project, requestId)
  }

  async get(actor: Actor, projectId: string): Promise<DraftView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) {
      throw notFound('Project')
    }
    const draft = await this.#drafts.findByProjectInternalId(project.id)
    if (!draft) {
      throw notFound('Draft')
    }
    return draft
  }

  async createRevision(
    actor: Actor,
    draftId: string,
    input: CreateDraftRevisionInput,
    requestId: string,
  ): Promise<DraftRevisionView> {
    const draft = await this.#drafts.findByPublicId(draftId)
    if (!draft) {
      throw notFound('Draft')
    }
    const project = await this.#projects.findIdentity(actor, draft.projectId)
    if (!project) {
      throw notFound('Draft')
    }
    await this.requireEditor(actor, bufferToPublicId(project.environment_public_id))
    const summary = input.changeSummary.trim()
    if (summary.length > 1024) {
      throw badRequest('INVALID_CHANGE_SUMMARY', 'changeSummary must not exceed 1024 characters')
    }
    return this.#drafts.createRevision(
      actor,
      draftId,
      parseLockVersion(input.lockVersion),
      parseProjectRouteModel(input.model),
      summary,
      requestId,
    )
  }

  async getRevision(actor: Actor, draftId: string, revisionId: string): Promise<DraftRevisionView> {
    const draft = await this.#drafts.findByPublicId(draftId)
    if (!draft) {
      throw notFound('Draft revision')
    }
    const project = await this.#projects.findIdentity(actor, draft.projectId)
    if (!project) {
      throw notFound('Draft revision')
    }
    const revision = await this.#drafts.getRevision(draftId, revisionId)
    if (!revision) {
      throw notFound('Draft revision')
    }
    return revision
  }

  async requireEditor(actor: Actor, environmentId: string): Promise<void> {
    const role = await this.#environments.role(actor, environmentId)
    if (!role) {
      throw notFound('Environment')
    }
    if (!['admin', 'maintainer'].includes(role)) {
      throw forbidden()
    }
  }
}

export class UnavailableDraftService implements DraftService {
  async get(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async getOrCreate(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async createRevision(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async getRevision(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
