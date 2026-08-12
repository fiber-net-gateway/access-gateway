import { forbidden, notFound, unavailable, unprocessable } from '../../shared/errors.js'
import { bufferToPublicId } from '../../shared/ids.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import type {
  BindProjectCertificateInput,
  CertificateListResult,
  CertificateView,
  CreateCertificateInput,
  ProjectCertificateBindingView,
} from './model.js'
import { certificateCoversDomain, parseCertificateUpload } from './parser.js'
import { CertificateRepository } from './repository.js'

export interface CertificateService {
  list(actor: Actor): Promise<CertificateListResult>
  create(actor: Actor, input: CreateCertificateInput, requestId: string): Promise<CertificateView>
  getProjectBinding(actor: Actor, projectId: string): Promise<ProjectCertificateBindingView>
  bindProject(
    actor: Actor,
    projectId: string,
    input: BindProjectCertificateInput,
    requestId: string,
  ): Promise<ProjectCertificateBindingView>
  unbindProject(
    actor: Actor,
    projectId: string,
    requestId: string,
  ): Promise<ProjectCertificateBindingView>
}

export class DefaultCertificateService implements CertificateService {
  readonly #certificates: CertificateRepository
  readonly #projects: ProjectRepository
  readonly #environments: EnvironmentRepository

  constructor(
    certificates: CertificateRepository,
    projects: ProjectRepository,
    environments: EnvironmentRepository,
  ) {
    this.#certificates = certificates
    this.#projects = projects
    this.#environments = environments
  }

  async list(actor: Actor): Promise<CertificateListResult> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) throw notFound('Deployment workspace')
    return { items: await this.#certificates.list(actor, environment.id) }
  }

  async create(
    actor: Actor,
    input: CreateCertificateInput,
    requestId: string,
  ): Promise<CertificateView> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) throw notFound('Deployment workspace')
    await this.requireMaintainer(actor, environment.id)
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    const parsed = parseCertificateUpload(input)
    const publicId = await this.#certificates.create(
      actor,
      environmentInternalId,
      parsed,
      requestId,
    )
    const created = await this.#certificates.findInEnvironment(environmentInternalId, publicId)
    if (!created) throw new Error('Created certificate could not be reloaded')
    const {
      internalId: _internalId,
      environmentInternalId: _environmentInternalId,
      ...view
    } = created
    return view
  }

  async getProjectBinding(actor: Actor, projectId: string): Promise<ProjectCertificateBindingView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) throw notFound('Project')
    return this.#certificates.getBinding(project)
  }

  async bindProject(
    actor: Actor,
    projectId: string,
    input: BindProjectCertificateInput,
    requestId: string,
  ): Promise<ProjectCertificateBindingView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) throw notFound('Project')
    await this.requireMaintainer(actor, bufferToPublicId(project.environment_public_id))
    const certificate = await this.#certificates.findInEnvironment(
      project.environment_id,
      input.certificateId,
    )
    if (!certificate) throw notFound('Certificate')
    if (certificate.status !== 'valid' && certificate.status !== 'expiring') {
      throw unprocessable(
        'CERTIFICATE_NOT_BINDABLE',
        'Only a currently valid certificate can be bound',
      )
    }
    if (!certificateCoversDomain(certificate.dnsNames, project.name)) {
      throw unprocessable(
        'CERTIFICATE_DOMAIN_NOT_COVERED',
        'The certificate DNS SANs do not cover the Project domain',
        [
          {
            path: 'certificateId',
            code: 'DOMAIN_NOT_COVERED',
            message: `${project.name} is not covered by this certificate`,
          },
        ],
      )
    }
    await this.#certificates.bind(actor, project, certificate.internalId, certificate.id, requestId)
    return this.#certificates.getBinding(project)
  }

  async unbindProject(
    actor: Actor,
    projectId: string,
    requestId: string,
  ): Promise<ProjectCertificateBindingView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) throw notFound('Project')
    await this.requireMaintainer(actor, bufferToPublicId(project.environment_public_id))
    await this.#certificates.unbind(actor, project, requestId)
    return this.#certificates.getBinding(project)
  }

  private async requireMaintainer(actor: Actor, environmentId: string): Promise<void> {
    const role = await this.#environments.role(actor, environmentId)
    if (!role) throw notFound('Deployment workspace')
    if (!['admin', 'maintainer'].includes(role)) throw forbidden()
  }
}

export class UnavailableCertificateService implements CertificateService {
  async list(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async create(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async getProjectBinding(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async bindProject(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async unbindProject(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
