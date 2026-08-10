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
import { isProjectRoutesModel } from './model.js'
import { DraftRepository } from './repository.js'
import { compileProjectRoutes, type RouteValidationIssue } from './compiler.js'

export interface ProjectRoutesValidationView {
  valid: boolean
  issues: readonly RouteValidationIssue[]
  wirePreview: string | null
  wireSha256: string | null
  validator: {
    contractVersion: number
    revision: string
  } | null
}

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
      'Draft model does not match project_routes_yaml schema version 2',
      [{ path: 'model', code: 'INVALID_SCHEMA', message: 'Invalid YAML route model' }],
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
      parseProjectRoutesModel(input.model),
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
    const result = compileProjectRoutes(project.name, parsed)
    if (!result.compiled) {
      return {
        valid: false,
        issues: result.issues,
        wirePreview: null,
        wireSha256: null,
        validator: null,
      }
    }
    const native = await this.#validator.validate(
      {
        requestId,
        kind: 'project_route',
        project: project.name,
        payload: result.compiled.payload,
      },
      signal,
    )
    const issues: RouteValidationIssue[] = native.errors.map((error) => {
      const match = /^routes\[(\d+)\](?:\.(.*))?$/u.exec(error.field ?? '')
      const route = match?.[1] ? parsed.routes[Number(match[1])] : undefined
      return {
        routeId: route?.id ?? parsed.routes[0]?.id ?? projectId,
        path: match?.[2] ?? error.field ?? '',
        line: 1,
        column: 1,
        code: error.code,
        message: error.message,
      }
    })
    return {
      valid: native.valid,
      issues,
      wirePreview: result.compiled.payloadText,
      wireSha256: result.compiled.sha256,
      validator: {
        contractVersion: native.contractVersion,
        revision: native.validatorRevision,
      },
    }
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
