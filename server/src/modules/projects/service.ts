import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import { normalizeExactHost } from '../../shared/hosts.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { CreateProjectInput, ProjectListResult, ProjectView } from './model.js'
import { ProjectRepository } from './repository.js'

export interface ProjectService {
  list(actor: Actor): Promise<ProjectListResult>
  get(actor: Actor, projectId: string): Promise<ProjectView>
  create(actor: Actor, input: CreateProjectInput, requestId: string): Promise<{ id: string }>
}

export function normalizeProjectDomain(value: string): string {
  const domain = normalizeExactHost(value)
  if (!domain) {
    throw badRequest(
      'INVALID_PROJECT_DOMAIN',
      'Project domain must be a valid DNS hostname without a scheme, port, path, wildcard, or IP literal',
      [{ path: 'domain', code: 'INVALID_FORMAT', message: 'Invalid project domain' }],
    )
  }
  return domain
}

export class DefaultProjectService implements ProjectService {
  readonly #projects: ProjectRepository
  readonly #environments: EnvironmentRepository

  constructor(projects: ProjectRepository, environments: EnvironmentRepository) {
    this.#projects = projects
    this.#environments = environments
  }

  async list(actor: Actor): Promise<ProjectListResult> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) {
      throw notFound('Deployment workspace')
    }
    return { items: await this.#projects.list(actor, environment.id) }
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
    input: CreateProjectInput,
    requestId: string,
  ): Promise<{ id: string }> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) {
      throw notFound('Deployment workspace')
    }
    const role = await this.#environments.role(actor, environment.id)
    if (!role) {
      throw notFound('Deployment workspace')
    }
    if (!['admin', 'maintainer'].includes(role)) {
      throw forbidden()
    }
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) {
      throw notFound('Deployment workspace')
    }
    return {
      id: await this.#projects.create(
        actor,
        environmentInternalId,
        normalizeProjectDomain(input.domain),
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
