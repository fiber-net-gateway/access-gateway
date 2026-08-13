import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import { bufferToPublicId } from '../../shared/ids.js'
import type { Actor } from '../auth/model.js'
import { compileProjectRoutes } from '../drafts/compiler.js'
import { isProjectRoutesModel, type ProjectRoutesModel } from '../drafts/model.js'
import { validateProjectRoutesCandidate } from '../drafts/validation.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import type {
  ConfigurationVersionDetail,
  ConfigurationVersionListResult,
  RestoreConfigurationVersionInput,
  SaveConfigurationVersionInput,
  SavedConfigurationVersion,
  ValidatedConfigurationVersion,
} from './model.js'
import { ConfigurationVersionRepository } from './repository.js'

export interface ConfigurationVersionService {
  list(
    actor: Actor,
    projectId: string,
    cursor?: string,
    limit?: number,
  ): Promise<ConfigurationVersionListResult>
  get(actor: Actor, projectId: string, versionId: string): Promise<ConfigurationVersionDetail>
  getCurrent(actor: Actor, projectId: string): Promise<SavedConfigurationVersion>
  save(
    actor: Actor,
    projectId: string,
    input: SaveConfigurationVersionInput,
    requestId: string,
  ): Promise<SavedConfigurationVersion>
  restore(
    actor: Actor,
    projectId: string,
    sourceVersionId: string,
    input: RestoreConfigurationVersionInput,
    requestId: string,
  ): Promise<SavedConfigurationVersion>
  validate(
    actor: Actor,
    projectId: string,
    versionId: string,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<ValidatedConfigurationVersion>
}

function parseModel(value: unknown): ProjectRoutesModel {
  if (!isProjectRoutesModel(value)) {
    throw badRequest(
      'INVALID_CONFIGURATION_MODEL',
      'The configuration does not match project_routes_yaml schema version 5',
      [{ path: 'model', code: 'INVALID_SCHEMA', message: 'Invalid mixed route model' }],
    )
  }
  return value
}

function parseSavableModel(domain: string, value: unknown): ProjectRoutesModel {
  const model = parseModel(value)
  const result = compileProjectRoutes(domain, model)
  if (!result.compiled) {
    const routeIndexes = new Map(model.routes.map((route, index) => [route.id, index]))
    throw badRequest(
      // Preserve the established machine-readable code for existing API clients. The
      // message and field paths now cover both YAML and JavaScript Route items.
      'INVALID_CONFIGURATION_YAML',
      'The configuration contains invalid routes or network policy',
      result.issues.map((issue) => {
        const routeIndex = routeIndexes.get(issue.routeId)
        return {
          path:
            routeIndex === undefined
              ? `model.${issue.path || 'networkPolicy'}`
              : `model.routes.${routeIndex}.${
                  model.routes[routeIndex]?.format === 'js' &&
                  (issue.path === 'path' || issue.path === 'method')
                    ? issue.path
                    : 'source'
                }`,
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
    throw badRequest('INVALID_LOCK_VERSION', 'The configuration lock version is invalid')
  }
  return value
}

function parseSummary(value: string): string {
  const summary = value.trim()
  if (summary.length < 1 || summary.length > 200) {
    throw badRequest(
      'INVALID_VERSION_SUMMARY',
      'A configuration version summary must contain 1-200 characters',
      [{ path: 'changeSummary', code: 'INVALID_LENGTH', message: 'Enter 1-200 characters' }],
    )
  }
  return summary
}

function decodeCursor(value: string | undefined): number | null {
  if (!value) return null
  let decoded: string
  try {
    decoded = Buffer.from(value, 'base64url').toString('utf8')
  } catch {
    throw badRequest('INVALID_CURSOR', 'The configuration version cursor is invalid')
  }
  if (!/^[1-9][0-9]*$/u.test(decoded)) {
    throw badRequest('INVALID_CURSOR', 'The configuration version cursor is invalid')
  }
  const number = Number(decoded)
  if (!Number.isSafeInteger(number)) {
    throw badRequest('INVALID_CURSOR', 'The configuration version cursor is invalid')
  }
  return number
}

function encodeCursor(number: number): string {
  return Buffer.from(String(number), 'utf8').toString('base64url')
}

export class DefaultConfigurationVersionService implements ConfigurationVersionService {
  readonly #versions: ConfigurationVersionRepository
  readonly #projects: ProjectRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator

  constructor(
    versions: ConfigurationVersionRepository,
    projects: ProjectRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
  ) {
    this.#versions = versions
    this.#projects = projects
    this.#environments = environments
    this.#validator = validator
  }

  async list(
    actor: Actor,
    projectId: string,
    cursor?: string,
    requestedLimit = 50,
  ): Promise<ConfigurationVersionListResult> {
    const project = await this.requireProject(actor, projectId)
    const limit = Math.min(Math.max(requestedLimit, 1), 100)
    const rows = await this.#versions.list(project.id, decodeCursor(cursor), limit + 1)
    const hasMore = rows.length > limit
    const items = hasMore ? rows.slice(0, limit) : rows
    const current = await this.#versions.findCurrentSummary(project.id)
    return {
      items,
      nextCursor: hasMore && items.length > 0 ? encodeCursor(items.at(-1)!.number) : null,
      currentVersionId: current?.id ?? null,
      lockVersion: await this.#versions.lockVersion(project.id),
    }
  }

  async get(
    actor: Actor,
    projectId: string,
    versionId: string,
  ): Promise<ConfigurationVersionDetail> {
    const project = await this.requireProject(actor, projectId)
    const version = await this.#versions.findDetail(project.id, versionId)
    if (!version) throw notFound('Configuration version')
    return version
  }

  async getCurrent(actor: Actor, projectId: string): Promise<SavedConfigurationVersion> {
    const project = await this.requireProject(actor, projectId)
    const current = await this.#versions.findCurrentSummary(project.id)
    if (!current) throw notFound('Configuration version')
    const version = await this.#versions.findDetail(project.id, current.id)
    if (!version) throw notFound('Configuration version')
    return { version, lockVersion: await this.#versions.lockVersion(project.id) }
  }

  async save(
    actor: Actor,
    projectId: string,
    input: SaveConfigurationVersionInput,
    requestId: string,
  ): Promise<SavedConfigurationVersion> {
    const project = await this.requireProject(actor, projectId)
    await this.requireMaintainer(actor, bufferToPublicId(project.environment_public_id))
    return this.#versions.save({
      actor,
      project,
      expectedLockVersion: parseLockVersion(input.lockVersion),
      baseVersionId: input.baseVersionId,
      changeSummary: parseSummary(input.changeSummary),
      forceSameContent: input.forceSameContent,
      idempotencyKey: input.idempotencyKey,
      model: parseSavableModel(project.name, input.model),
      requestId,
    })
  }

  async restore(
    actor: Actor,
    projectId: string,
    sourceVersionId: string,
    input: RestoreConfigurationVersionInput,
    requestId: string,
  ): Promise<SavedConfigurationVersion> {
    const project = await this.requireProject(actor, projectId)
    await this.requireMaintainer(actor, bufferToPublicId(project.environment_public_id))
    const source = await this.#versions.findDetail(project.id, sourceVersionId)
    if (!source) throw notFound('Configuration version')
    return this.#versions.save({
      actor,
      project,
      expectedLockVersion: parseLockVersion(input.lockVersion),
      baseVersionId: input.baseVersionId,
      changeSummary: parseSummary(input.changeSummary),
      forceSameContent: input.forceSameContent,
      idempotencyKey: input.idempotencyKey,
      model: parseSavableModel(
        project.name,
        input.model === undefined ? source.model : input.model,
      ),
      restoredFromVersionId: source.id,
      requestId,
    })
  }

  async validate(
    actor: Actor,
    projectId: string,
    versionId: string,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<ValidatedConfigurationVersion> {
    const project = await this.requireProject(actor, projectId)
    await this.requireMaintainer(actor, bufferToPublicId(project.environment_public_id))
    const version = await this.#versions.findStored(project.id, versionId)
    if (!version) throw notFound('Configuration version')
    const validation = await validateProjectRoutesCandidate(
      this.#validator,
      project.name,
      version.id,
      version.model,
      requestId,
      1,
      signal,
    )
    await this.#versions.recordValidation(actor, project, version, validation, requestId)
    const updated = await this.#versions.findDetail(project.id, version.id)
    if (!updated) throw notFound('Configuration version')
    return { version: updated, validation }
  }

  private async requireProject(actor: Actor, projectId: string) {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) throw notFound('Project')
    return project
  }

  private async requireMaintainer(actor: Actor, environmentId: string): Promise<void> {
    const role = await this.#environments.role(actor, environmentId)
    if (!role) throw notFound('Project')
    if (!['admin', 'maintainer'].includes(role)) throw forbidden()
  }
}

export class UnavailableConfigurationVersionService implements ConfigurationVersionService {
  async list(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async get(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async getCurrent(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async save(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async restore(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async validate(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
