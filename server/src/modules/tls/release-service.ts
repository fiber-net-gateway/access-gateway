import type { NacosClient, NacosTarget } from '../../integrations/nacos/model.js'
import { forbidden, notFound, unavailable } from '../../shared/errors.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type {
  CreateTlsCertificateReleaseInput,
  QueueTlsCertificatePublicationResult,
  TlsCertificateReleaseView,
} from './release-model.js'
import {
  TLS_CERTIFICATES_DATA_ID,
  TLS_CERTIFICATES_GROUP,
  TlsCertificateReleaseRepository,
} from './release-repository.js'

export interface TlsCertificateReleaseService {
  create(
    actor: Actor,
    input: CreateTlsCertificateReleaseInput,
    requestId: string,
  ): Promise<TlsCertificateReleaseView>
  list(actor: Actor): Promise<{ items: readonly TlsCertificateReleaseView[] }>
  get(actor: Actor, releaseId: string): Promise<TlsCertificateReleaseView>
  queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueTlsCertificatePublicationResult>
}

export class DefaultTlsCertificateReleaseService implements TlsCertificateReleaseService {
  readonly #releases: TlsCertificateReleaseRepository
  readonly #environments: EnvironmentRepository
  readonly #nacos: NacosClient

  constructor(
    releases: TlsCertificateReleaseRepository,
    environments: EnvironmentRepository,
    nacos: NacosClient,
  ) {
    this.#releases = releases
    this.#environments = environments
    this.#nacos = nacos
  }

  async create(
    actor: Actor,
    input: CreateTlsCertificateReleaseInput,
    requestId: string,
  ): Promise<TlsCertificateReleaseView> {
    const environment = await this.requireEnvironment(actor)
    await this.requirePublisher(actor, environment.id)
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    const target: NacosTarget = {
      endpoint: environment.nacos.endpoint,
      namespace: environment.nacos.namespace,
      tenant: environment.nacos.tenant,
      credentialConfigured: environment.nacos.credentialConfigured,
    }
    const base = await this.#nacos.read(target, TLS_CERTIFICATES_DATA_ID, TLS_CERTIFICATES_GROUP)
    return this.#releases.create(
      actor,
      environmentInternalId,
      input.defaultCertificateId,
      input.idempotencyKey,
      sha256(canonicalJson({ defaultCertificateId: input.defaultCertificateId })),
      base,
      requestId,
    )
  }

  async list(actor: Actor): Promise<{ items: readonly TlsCertificateReleaseView[] }> {
    const environment = await this.requireEnvironment(actor)
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    return { items: await this.#releases.list(environmentInternalId) }
  }

  async get(actor: Actor, releaseId: string): Promise<TlsCertificateReleaseView> {
    const environment = await this.requireEnvironment(actor)
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    const release = await this.#releases.find(releaseId, environmentInternalId)
    if (!release) throw notFound('TLS release')
    return release
  }

  async queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueTlsCertificatePublicationResult> {
    const environment = await this.requireEnvironment(actor)
    await this.requirePublisher(actor, environment.id)
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    const environmentInternalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!environmentInternalId) throw notFound('Deployment workspace')
    const release = await this.#releases.find(releaseId, environmentInternalId)
    if (!release) throw notFound('TLS release')
    return this.#releases.queuePublication(actor, releaseId, idempotencyKey, requestId)
  }

  private async requireEnvironment(actor: Actor) {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) throw notFound('Deployment workspace')
    return environment
  }

  private async requirePublisher(actor: Actor, environmentId: string): Promise<void> {
    const role = await this.#environments.role(actor, environmentId)
    if (!role) throw notFound('Deployment workspace')
    if (!['admin', 'publisher'].includes(role)) throw forbidden()
  }
}

export class UnavailableTlsCertificateReleaseService implements TlsCertificateReleaseService {
  async create(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
  async list(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
  async get(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
  async queuePublication(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
