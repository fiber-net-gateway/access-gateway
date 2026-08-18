import type {
  NacosClient,
  NacosResourceValue,
  NacosTarget,
} from '../../integrations/nacos/model.js'
import type { NativeValidator } from '../../integrations/native-validator/model.js'
import type { AccessConfigLimits } from '../../integrations/native-validator/model.js'
import {
  fallbackAccessConfigLimits,
  utf8Bytes,
} from '../../integrations/native-validator/limits.js'
import {
  AppError,
  conflict,
  forbidden,
  notFound,
  unavailable,
  unprocessable,
} from '../../shared/errors.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import { bufferToPublicId } from '../../shared/ids.js'
import { normalizeExactHost } from '../../shared/hosts.js'
import type { Actor } from '../auth/model.js'
import { compileProjectRoutes, ROUTE_COMPILER_REVISION } from '../drafts/compiler.js'
import { validateProjectRoutesCandidate } from '../drafts/validation.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import { ConfigurationVersionRepository } from '../versions/repository.js'
import type {
  CreateProjectReleaseInput,
  CreateProjectDecommissionReleaseInput,
  ProjectReleaseListResult,
  ProjectReleaseView,
  QueuePublicationResult,
} from './model.js'
import { ReleaseRepository, type PreparedReleaseResource } from './repository.js'

export interface ReleaseService {
  create(
    actor: Actor,
    projectId: string,
    input: CreateProjectReleaseInput,
    requestId: string,
  ): Promise<ProjectReleaseView>
  createDecommission(
    actor: Actor,
    projectId: string,
    input: CreateProjectDecommissionReleaseInput,
    requestId: string,
  ): Promise<ProjectReleaseView>
  list(actor: Actor, projectId: string): Promise<ProjectReleaseListResult>
  get(actor: Actor, releaseId: string): Promise<ProjectReleaseView>
  queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueuePublicationResult>
}

function validateTitle(value: string): string {
  const title = value.trim()
  if (title.length < 1 || title.length > 255) {
    throw unprocessable('INVALID_RELEASE_TITLE', 'Release title must contain 1-255 characters')
  }
  return title
}

function validateDescription(value: string): string {
  const description = value.trim()
  if (description.length > 4_000) {
    throw unprocessable(
      'INVALID_RELEASE_DESCRIPTION',
      'Release description must not exceed 4000 characters',
    )
  }
  return description
}

function projectNames(content: string | null): string[] {
  if (!content) return []
  return [
    ...new Set(
      content
        .split(';')
        .map((item) => item.trim())
        .filter(Boolean),
    ),
  ]
}

function routeHostPatterns(content: string | null): readonly string[] {
  if (!content) return []
  let parsed: unknown
  try {
    parsed = JSON.parse(content)
  } catch {
    throw new Error('route resource is not valid JSON')
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new Error('route resource must be a JSON object')
  }
  const host = (parsed as Record<string, unknown>).host
  if (typeof host !== 'object' || host === null || Array.isArray(host)) {
    throw new Error('route resource host map is missing')
  }
  return Object.keys(host)
}

const hostPreflightConcurrency = 16

export function hostPatternConflicts(pattern: string, alias: string): boolean {
  const normalizedAlias = normalizeExactHost(alias)
  if (!normalizedAlias) return false
  const normalizedPattern = normalizeExactHost(pattern)
  if (normalizedPattern) return normalizedPattern === normalizedAlias
  // Existing Java configurations may contain a leading-label wildcard. The
  // new editor does not create them, but an alias covered by one would still
  // be ambiguous across Projects, so reserve that namespace as well.
  if (pattern === '*') return true
  if (pattern.startsWith('*.')) {
    const suffix = normalizeExactHost(pattern.slice(2))
    return suffix !== null && normalizedAlias.endsWith(`.${suffix}`) && normalizedAlias !== suffix
  }
  return false
}

async function ensureHostAliasesAvailable(
  nacos: NacosClient,
  target: NacosTarget,
  routePrefix: string,
  routeGroup: string,
  projectName: string,
  projectList: string | null,
  aliases: readonly string[],
): Promise<void> {
  if (aliases.length === 0) return
  const otherProjects = projectNames(projectList).filter((name) => name !== projectName)
  if (otherProjects.length === 0) return
  const resources: NacosResourceValue[] = []
  for (let offset = 0; offset < otherProjects.length; offset += hostPreflightConcurrency) {
    const batch = otherProjects.slice(offset, offset + hostPreflightConcurrency)
    resources.push(
      ...(await Promise.all(
        batch.map((name) => nacos.read(target, `${routePrefix}${name}`, routeGroup)),
      )),
    )
  }
  for (const [index, resource] of resources.entries()) {
    if (!resource.exists || resource.content === null) {
      throw new Error(`route resource for ${otherProjects[index] ?? 'unknown'} is unavailable`)
    }
    const patterns = routeHostPatterns(resource.content)
    for (const alias of aliases) {
      const conflictPattern = patterns.find((pattern) => hostPatternConflicts(pattern, alias))
      if (conflictPattern) {
        throw unprocessable(
          'HOST_ALIAS_CONFLICT',
          'One or more associated domains are already bound to another Project',
          [
            {
              path: 'model.hostAliases',
              code: 'HOST_ALREADY_BOUND',
              message: `${alias} conflicts with an existing host binding`,
            },
          ],
        )
      }
    }
  }
}

function trimJava(value: string): string {
  let begin = 0
  let end = value.length
  while (begin < end && value.charCodeAt(begin) <= 0x20) begin += 1
  while (end > begin && value.charCodeAt(end - 1) <= 0x20) end -= 1
  return value.slice(begin, end)
}

function projectListLimitExceeded(path: string, message: string): never {
  throw unprocessable('PROJECT_LIST_LIMIT_EXCEEDED', 'The Project List exceeds native limits', [
    { path, code: 'limit_exceeded', message },
  ])
}

export function validateProjectListTarget(
  content: string,
  limits: AccessConfigLimits = fallbackAccessConfigLimits,
): string {
  const listLimits = limits.projectList
  if (utf8Bytes(content) > listLimits.maxPayloadBytes) {
    projectListLimitExceeded(
      'projectList',
      `Project List payload must not exceed ${listLimits.maxPayloadBytes} bytes`,
    )
  }
  const trimmed = trimJava(content)
  if (trimmed.length === 0) return content

  let index = 0
  let offset = 0
  while (true) {
    if (index === listLimits.maxProjects) {
      let onlyTrailingSeparators = true
      for (let cursor = offset; cursor < trimmed.length; cursor += 1) {
        if (trimmed[cursor] !== ';') {
          onlyTrailingSeparators = false
          break
        }
      }
      if (onlyTrailingSeparators) break
      projectListLimitExceeded(
        'projectList',
        `Project List must not contain more than ${listLimits.maxProjects} entries`,
      )
    }
    const separator = trimmed.indexOf(';', offset)
    const name = separator === -1 ? trimmed.slice(offset) : trimmed.slice(offset, separator)
    if (utf8Bytes(name) > listLimits.maxProjectNameBytes) {
      projectListLimitExceeded(
        `projectList.${index}`,
        `Project names must not exceed ${listLimits.maxProjectNameBytes} bytes`,
      )
    }
    index += 1
    if (separator === -1) break
    offset = separator + 1
  }
  return content
}

export function compileDecommissionProjectList(
  content: string | null,
  domain: string,
  limits: AccessConfigLimits = fallbackAccessConfigLimits,
): string {
  validateProjectListTarget(content ?? '', limits)
  const names = projectNames(content)
  const target = !names.includes(domain)
    ? (content ?? '')
    : names
        .filter((name) => name !== domain)
        .sort()
        .join(';')
  return validateProjectListTarget(target, limits)
}

function validateDecommissionReason(value: string): string {
  const reason = value.trim()
  if (reason.length < 1 || reason.length > 1_000) {
    throw unprocessable(
      'INVALID_DECOMMISSION_REASON',
      'Decommission reason must contain 1-1000 characters',
      [
        {
          path: 'reason',
          code: 'INVALID_LENGTH',
          message: 'Reason must contain 1-1000 characters',
        },
      ],
    )
  }
  return reason
}

export class DefaultReleaseService implements ReleaseService {
  readonly #releases: ReleaseRepository
  readonly #versions: ConfigurationVersionRepository
  readonly #projects: ProjectRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator
  readonly #nacos: NacosClient

  constructor(
    releases: ReleaseRepository,
    versions: ConfigurationVersionRepository,
    projects: ProjectRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
    nacos: NacosClient,
  ) {
    this.#releases = releases
    this.#versions = versions
    this.#projects = projects
    this.#environments = environments
    this.#validator = validator
    this.#nacos = nacos
  }

  async create(
    actor: Actor,
    projectId: string,
    input: CreateProjectReleaseInput,
    requestId: string,
  ): Promise<ProjectReleaseView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) throw notFound('Project')
    await this.requirePublisher(actor, project.environment_public_id)
    if (!this.#validator.available || !this.#validator.revision || !this.#validator.limits) {
      throw unavailable(
        'NATIVE_VALIDATOR_UNCONFIGURED',
        'Native Validator is required before creating a Release',
      )
    }
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    const source = await this.#versions.findStored(project.id, input.sourceVersionId)
    if (!source) throw notFound('Configuration version')
    const title = validateTitle(input.title)
    const description = validateDescription(input.description)
    const requestDigest = sha256(
      canonicalJson({
        projectId,
        sourceVersionId: input.sourceVersionId,
        expectedCurrentVersionId: input.expectedCurrentVersionId,
        title,
        description,
      }),
    )
    const begun = await this.#releases.beginCreate({
      actor,
      project,
      source,
      expectedCurrentVersionId: input.expectedCurrentVersionId,
      title,
      description,
      idempotencyKey: input.idempotencyKey,
      requestSha256: requestDigest,
      compilerRevision: ROUTE_COMPILER_REVISION,
      validatorContractVersion: this.#validator.contractVersion,
      validatorRevision: this.#validator.revision,
      requestId,
    })
    if (begun.replay) return begun.release

    const wireVersion = begun.release.allocatedWireVersion
    if (begun.release.kind !== 'project_route' || wireVersion === null) {
      throw new Error('Prepared route Release is missing its allocated wire version')
    }
    const validation = await validateProjectRoutesCandidate(
      this.#validator,
      project.name,
      source.id,
      source.model,
      requestId,
      wireVersion,
    )
    await this.#versions.recordValidation(actor, project, source, validation, requestId)
    if (!validation.valid) {
      await this.#releases.markValidationFailed(begun.release.id, validation.issues)
      throw unprocessable(
        'CONFIG_VERSION_NOT_PUBLISHABLE',
        'The selected configuration version failed current validation',
        validation.issues.map((issue) => ({
          path: `routes.${issue.routeId}${issue.path ? `.${issue.path}` : ''}`,
          code: issue.code,
          message: issue.message,
        })),
      )
    }
    const compiled = compileProjectRoutes(
      project.name,
      source.model,
      wireVersion,
      this.#validator.limits,
    ).compiled
    if (!compiled) throw new Error('Validated route configuration did not compile')

    const environment = await this.#environments.findAccessibleByPublicId(
      actor,
      bufferToPublicId(project.environment_public_id),
    )
    if (!environment) throw notFound('Workspace')
    const target: NacosTarget = {
      endpoint: environment.nacos.endpoint,
      namespace: environment.nacos.namespace,
      tenant: environment.nacos.tenant,
      credentialConfigured: environment.nacos.credentialConfigured,
    }
    const routeDataId = `${environment.dataIds.routePrefix}${project.name}`
    let bases: [NacosResourceValue, NacosResourceValue]
    try {
      bases = await Promise.all([
        this.#nacos.read(target, routeDataId, environment.dataIds.routeGroup),
        this.#nacos.read(target, environment.dataIds.projects, environment.dataIds.routeGroup),
      ])
    } catch (error) {
      await this.#releases.markAbandoned(begun.release.id, 'NACOS_PREFLIGHT_FAILED')
      throw error
    }
    const [routeBase, projectsBase] = bases

    const resources: PreparedReleaseResource[] = [
      {
        kind: 'project_route',
        dataId: routeDataId,
        group: environment.dataIds.routeGroup,
        contentType: 'json',
        targetContent: compiled.payloadText,
        base: routeBase,
        publishOrder: 10,
      },
    ]
    try {
      validateProjectListTarget(projectsBase.content ?? '', this.#validator.limits)
      const names = projectNames(projectsBase.content)
      if (!names.includes(project.name)) {
        resources.push({
          kind: 'project_list',
          dataId: environment.dataIds.projects,
          group: environment.dataIds.routeGroup,
          contentType: 'text',
          targetContent: validateProjectListTarget(
            [...names, project.name].sort().join(';'),
            this.#validator.limits,
          ),
          base: projectsBase,
          publishOrder: 20,
          dependsOnKind: 'project_route',
        })
      }
    } catch (error) {
      await this.#releases.markAbandoned(begun.release.id, 'PROJECT_LIST_LIMIT_EXCEEDED')
      throw error
    }
    try {
      await ensureHostAliasesAvailable(
        this.#nacos,
        target,
        environment.dataIds.routePrefix,
        environment.dataIds.routeGroup,
        project.name,
        projectsBase.content,
        source.model.hostAliases,
      )
    } catch (error) {
      await this.#releases.markAbandoned(begun.release.id, 'HOST_ALIAS_PREFLIGHT_FAILED')
      if (error instanceof AppError && error.code === 'HOST_ALIAS_CONFLICT') throw error
      throw unavailable(
        'HOST_ALIAS_PREFLIGHT_FAILED',
        'Existing Project host bindings could not be checked before publication',
      )
    }
    try {
      await this.#releases.completePreparation(
        begun.release.id,
        project,
        source,
        wireVersion,
        resources,
        {
          sourceVersionId: source.id,
          sourceVersionNumber: source.number,
          sourceModelSha256: source.modelSha256,
          allocatedWireVersion: wireVersion,
          routeCount: source.model.routes.length,
          routeBaseSha256: routeBase.sha256,
          routeTargetSha256: compiled.sha256,
          addsProjectToList: resources.some((resource) => resource.kind === 'project_list'),
        },
      )
    } catch (error) {
      await this.#releases.markAbandoned(begun.release.id, 'RELEASE_PREPARATION_FAILED')
      throw error
    }
    const release = await this.#releases.findByPublicId(begun.release.id)
    if (!release) throw notFound('Release')
    return release
  }

  async createDecommission(
    actor: Actor,
    projectId: string,
    input: CreateProjectDecommissionReleaseInput,
    requestId: string,
  ): Promise<ProjectReleaseView> {
    const project = await this.#projects.findIdentityForHistory(actor, projectId)
    if (!project) throw notFound('Project')
    if (project.status === 'archived') {
      throw conflict('PROJECT_ARCHIVED', 'An archived Project cannot be decommissioned again')
    }
    await this.requireAdmin(actor, project.environment_public_id)
    if (input.confirmationDomain.trim() !== project.name) {
      throw unprocessable(
        'PROJECT_CONFIRMATION_MISMATCH',
        'The confirmation domain must exactly match the Project domain',
        [
          {
            path: 'confirmationDomain',
            code: 'MISMATCH',
            message: `Enter ${project.name} to confirm`,
          },
        ],
      )
    }
    const reason = validateDecommissionReason(input.reason)
    if (!this.#validator.available || !this.#validator.revision || !this.#validator.limits) {
      throw unavailable(
        'NATIVE_VALIDATOR_UNCONFIGURED',
        'Native Validator is required before creating a decommission Release',
      )
    }
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    const environment = await this.#environments.findAccessibleByPublicId(
      actor,
      bufferToPublicId(project.environment_public_id),
    )
    if (!environment) throw notFound('Workspace')
    const target: NacosTarget = {
      endpoint: environment.nacos.endpoint,
      namespace: environment.nacos.namespace,
      tenant: environment.nacos.tenant,
      credentialConfigured: environment.nacos.credentialConfigured,
    }
    let base: NacosResourceValue
    try {
      base = await this.#nacos.read(
        target,
        environment.dataIds.projects,
        environment.dataIds.routeGroup,
      )
    } catch {
      throw unavailable(
        'NACOS_PREFLIGHT_FAILED',
        'The Project List could not be read before creating the decommission Release',
      )
    }
    const plan = {
      schemaVersion: 1,
      kind: 'project_decommission',
      projectId,
      domain: project.name,
      reason,
    } as const
    const requestSha256 = sha256(
      canonicalJson({
        ...plan,
        expectedLockVersion: input.expectedLockVersion,
      }),
    )
    const result = await this.#releases.beginDecommission({
      actor,
      project,
      expectedLockVersion: input.expectedLockVersion,
      reason,
      idempotencyKey: input.idempotencyKey,
      requestSha256,
      requestId,
      planText: canonicalJson(plan),
      dataId: environment.dataIds.projects,
      group: environment.dataIds.routeGroup,
      base,
      targetContent: compileDecommissionProjectList(
        base.content,
        project.name,
        this.#validator.limits,
      ),
    })
    return result.release
  }

  async list(actor: Actor, projectId: string): Promise<ProjectReleaseListResult> {
    const project = await this.#projects.findIdentityForHistory(actor, projectId)
    if (!project) throw notFound('Project')
    return { items: await this.#releases.listByProject(project.id) }
  }

  async get(actor: Actor, releaseId: string): Promise<ProjectReleaseView> {
    const release = await this.#releases.findByPublicId(releaseId)
    if (!release) throw notFound('Release')
    const project = await this.#projects.findIdentityForHistory(actor, release.projectId)
    if (!project) throw notFound('Release')
    return release
  }

  async queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueuePublicationResult> {
    const release = await this.get(actor, releaseId)
    const project = await this.#projects.findIdentityForHistory(actor, release.projectId)
    if (!project) throw notFound('Release')
    await this.requirePublisher(actor, project.environment_public_id)
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    return this.#releases.queuePublication(actor, releaseId, idempotencyKey, requestId)
  }

  private async requirePublisher(actor: Actor, environmentPublicId: Buffer): Promise<void> {
    const environmentId = bufferToPublicId(environmentPublicId)
    const role = await this.#environments.role(actor, environmentId)
    if (!role) throw notFound('Project')
    if (!['admin', 'publisher'].includes(role)) throw forbidden()
  }

  private async requireAdmin(actor: Actor, environmentPublicId: Buffer): Promise<void> {
    const environmentId = bufferToPublicId(environmentPublicId)
    const role = await this.#environments.role(actor, environmentId)
    if (!role) throw notFound('Project')
    if (role !== 'admin') throw forbidden()
  }
}

export class UnavailableReleaseService implements ReleaseService {
  async create(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async list(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async createDecommission(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async get(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async queuePublication(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
