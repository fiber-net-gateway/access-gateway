import { badRequest, forbidden, notFound, unavailable, unprocessable } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { ProjectRepository } from '../projects/repository.js'
import type {
  CertificateListResult,
  CertificateVersionListResult,
  CertificateView,
  CreateCertificateInput,
  CreateCertificateVersionInput,
  ProjectCertificateResolutionView,
} from './model.js'
import { missingManagedDnsNames, parseCertificateUpload } from './parser.js'
import { CertificateRepository } from './repository.js'

function parseLockVersion(value: string): string {
  if (!/^(0|[1-9][0-9]*)$/u.test(value)) {
    throw badRequest('INVALID_LOCK_VERSION', 'lockVersion must be an unsigned integer string')
  }
  return value
}

export interface CertificateService {
  list(actor: Actor): Promise<CertificateListResult>
  create(actor: Actor, input: CreateCertificateInput, requestId: string): Promise<CertificateView>
  createVersion(
    actor: Actor,
    certificateId: string,
    input: CreateCertificateVersionInput,
    expectedLockVersion: string,
    requestId: string,
  ): Promise<CertificateView>
  listVersions(actor: Actor, certificateId: string): Promise<CertificateVersionListResult>
  resolveProject(actor: Actor, projectId: string): Promise<ProjectCertificateResolutionView>
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
    return this.publicView(await this.requireCertificate(environmentInternalId, publicId))
  }

  async createVersion(
    actor: Actor,
    certificateId: string,
    input: CreateCertificateVersionInput,
    expectedLockVersion: string,
    requestId: string,
  ): Promise<CertificateView> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) throw notFound('Deployment workspace')
    await this.requireMaintainer(actor, environment.id)
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    const certificate = await this.requireCertificate(environmentInternalId, certificateId)
    const parsed = parseCertificateUpload({ name: certificate.name, ...input })
    const missingNames = missingManagedDnsNames(parsed.dnsNames, certificate.managedDnsNames)
    if (missingNames.length > 0) {
      throw unprocessable(
        'CERTIFICATE_MANAGED_NAMES_NOT_COVERED',
        'The new certificate version does not preserve every managed DNS selector',
        missingNames.map((dnsName) => ({
          path: 'certificatePem',
          code: 'MANAGED_DNS_NAME_NOT_COVERED',
          message: `${dnsName} is not covered by the new certificate version`,
        })),
      )
    }
    await this.#certificates.createVersion(
      actor,
      certificate.internalId,
      environmentInternalId,
      certificate.id,
      certificate.name,
      parseLockVersion(expectedLockVersion),
      parsed,
      requestId,
    )
    return this.publicView(await this.requireCertificate(environmentInternalId, certificateId))
  }

  async listVersions(actor: Actor, certificateId: string): Promise<CertificateVersionListResult> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) throw notFound('Deployment workspace')
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    const certificate = await this.requireCertificate(environmentInternalId, certificateId)
    return { items: await this.#certificates.listVersions(certificate.internalId) }
  }

  async resolveProject(actor: Actor, projectId: string): Promise<ProjectCertificateResolutionView> {
    const project = await this.#projects.findIdentity(actor, projectId)
    if (!project) throw notFound('Project')
    return this.#certificates.resolveProject(project)
  }

  private async requireCertificate(
    environmentInternalId: string,
    certificateId: string,
  ): Promise<
    CertificateView & {
      internalId: string
      environmentInternalId: string
    }
  > {
    const certificate = await this.#certificates.findInEnvironment(
      environmentInternalId,
      certificateId,
    )
    if (!certificate) throw notFound('Certificate')
    return certificate
  }

  private publicView(
    certificate: CertificateView & {
      internalId: string
      environmentInternalId: string
    },
  ): CertificateView {
    const {
      internalId: _internalId,
      environmentInternalId: _environmentInternalId,
      ...view
    } = certificate
    return view
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

  async createVersion(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async listVersions(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async resolveProject(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
