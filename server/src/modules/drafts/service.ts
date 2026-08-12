import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { bufferToPublicId } from '../../shared/ids.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import type {
  CreateDraftRevisionInput,
  DraftRevisionView,
  DraftView,
  ProjectRoutesModel,
} from './model.js'
import { compileProjectRoutes } from './compiler.js'
import { isProjectRoutesModel } from './model.js'
import { DraftRepository } from './repository.js'
import { validateProjectRoutesCandidate, type ProjectRoutesValidationView } from './validation.js'

export type { ProjectRoutesValidationView } from './validation.js'

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
  getCurrentRevision(actor: Actor, draftId: string): Promise<DraftRevisionView>
  validate(
    actor: Actor,
    projectId: string,
    model: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<ProjectRoutesValidationView>
}

function parseProjectRoutesModel(value: unknown): ProjectRoutesModel {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw badRequest('INVALID_DRAFT_MODEL', 'Draft model must be an object')
  }
  if (!isProjectRoutesModel(value)) {
    throw badRequest(
      'INVALID_DRAFT_MODEL',
      'Draft model does not match project_routes_yaml schema version 3',
      [{ path: 'model', code: 'INVALID_SCHEMA', message: 'Invalid YAML route model' }],
    )
  }
  return value
}

function parseSavableProjectRoutesModel(domain: string, value: unknown): ProjectRoutesModel {
  const model = parseProjectRoutesModel(value)
  const result = compileProjectRoutes(domain, model)
  if (!result.compiled) {
    const routeIndexes = new Map(model.routes.map((route, index) => [route.id, index]))
    throw badRequest(
      'INVALID_CONFIGURATION_YAML',
      'The configuration contains invalid routes or network policy',
      result.issues.map((issue) => {
        const routeIndex = routeIndexes.get(issue.routeId)
        return {
          path:
            routeIndex === undefined
              ? `model.${issue.path || 'networkPolicy'}`
              : `model.routes.${routeIndex}.source`,
          code: issue.code,
          message: `${issue.message} (${issue.line}:${issue.column})`,
        }
      }),
    )
  }
  return model
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
  readonly #validator: NativeValidator

  constructor(
    drafts: DraftRepository,
    projects: ProjectRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
  ) {
    this.#drafts = drafts
    this.#projects = projects
    this.#environments = environments
    this.#validator = validator
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
      parseSavableProjectRoutesModel(project.name, input.model),
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

  async getCurrentRevision(actor: Actor, draftId: string): Promise<DraftRevisionView> {
    const draft = await this.#drafts.findByPublicId(draftId)
    if (!draft) {
      throw notFound('Draft revision')
    }
    const project = await this.#projects.findIdentity(actor, draft.projectId)
    if (!project) {
      throw notFound('Draft revision')
    }
    const revision = await this.#drafts.getCurrentRevision(draftId)
    if (!revision) {
      throw notFound('Draft revision')
    }
    return revision
  }

  async validate(
    actor: Actor,
    projectId: string,
    model: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<ProjectRoutesValidationView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) {
      throw notFound('Project')
    }
    await this.requireEditor(actor, bufferToPublicId(project.environment_public_id))
    const parsed = parseProjectRoutesModel(model)
    return validateProjectRoutesCandidate(
      this.#validator,
      project.name,
      projectId,
      parsed,
      requestId,
      1,
      signal,
    )
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

  async getCurrentRevision(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async validate(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
