import type {
  NacosClient,
  NacosResourceValue,
  NacosTarget,
} from '../../integrations/nacos/model.js'
import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { conflict, forbidden, notFound, unavailable, unprocessable } from '../../shared/errors.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import { bufferToPublicId } from '../../shared/ids.js'
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

export function compileDecommissionProjectList(content: string | null, domain: string): string {
  const names = projectNames(content)
  if (!names.includes(domain)) return content ?? ''
  return names
    .filter((name) => name !== domain)
    .sort()
    .join(';')
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
    if (!this.#validator.available || !this.#validator.revision) {
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
    const compiled = compileProjectRoutes(project.name, source.model, wireVersion).compiled
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
    const names = projectNames(projectsBase.content)
    if (!names.includes(project.name)) {
      resources.push({
        kind: 'project_list',
        dataId: environment.dataIds.projects,
        group: environment.dataIds.routeGroup,
        contentType: 'text',
        targetContent: [...names, project.name].sort().join(';'),
        base: projectsBase,
        publishOrder: 20,
        dependsOnKind: 'project_route',
      })
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
      targetContent: compileDecommissionProjectList(base.content, project.name),
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
