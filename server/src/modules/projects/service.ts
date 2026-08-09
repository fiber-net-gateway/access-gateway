import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { CreateProjectInput, ProjectListResult, ProjectView } from './model.js'
import { ProjectRepository } from './repository.js'

export interface ProjectService {
  list(actor: Actor, environmentId: string): Promise<ProjectListResult>
  get(actor: Actor, projectId: string): Promise<ProjectView>
  create(
    actor: Actor,
    environmentId: string,
    input: CreateProjectInput,
    requestId: string,
  ): Promise<{ id: string }>
}

function validateProjectName(value: string): string {
  const name = value.trim()
  if (name.length === 0 || name.length > 255 || name.includes(';')) {
    throw badRequest(
      'INVALID_PROJECT_NAME',
      'Project name must be 1-255 characters and must not contain semicolons',
      [{ path: 'name', code: 'INVALID_FORMAT', message: 'Invalid project name' }],
    )
  }
  return name
}

export class DefaultProjectService implements ProjectService {
  readonly #projects: ProjectRepository
  readonly #environments: EnvironmentRepository

  constructor(projects: ProjectRepository, environments: EnvironmentRepository) {
    this.#projects = projects
    this.#environments = environments
  }

  async list(actor: Actor, environmentId: string): Promise<ProjectListResult> {
    const environment = await this.#environments.findAccessibleByPublicId(actor, environmentId)
    if (!environment) {
      throw notFound('Environment')
    }
    return { items: await this.#projects.list(actor, environmentId) }
  }

  async get(actor: Actor, projectId: string): Promise<ProjectView> {
    const project = await this.#projects.findView(actor, projectId)
    if (!project) {
      throw notFound('Project')
    }
    return project
  }

  async create(
    actor: Actor,
    environmentId: string,
    input: CreateProjectInput,
    requestId: string,
  ): Promise<{ id: string }> {
    const role = await this.#environments.role(actor, environmentId)
    if (!role) {
      throw notFound('Environment')
    }
    if (!['admin', 'maintainer'].includes(role)) {
      throw forbidden()
    }
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environmentId)
    if (!environmentInternalId) {
      throw notFound('Environment')
    }
    return {
      id: await this.#projects.create(
        actor,
        environmentInternalId,
        environmentId,
        validateProjectName(input.name),
        requestId,
      ),
    }
  }
}

export class UnavailableProjectService implements ProjectService {
  async list(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async create(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async get(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
