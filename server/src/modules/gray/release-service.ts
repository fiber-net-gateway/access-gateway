// Gray Match release service: compiles the saved gray rules draft into the
// `ploto.unified-access.gray-match` wire JSON, validates fail-closed with the
// Native Validator (`kind: 'gray_rules'`), reads the rnacos base observation,
// and persists an immutable Release. Rollback creates a NEW immutable Release
// from a historical release's stored payload without rewriting history.

import type {
  NacosClient,
  NacosResourceValue,
  NacosTarget,
} from '../../integrations/nacos/model.js'
import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { forbidden, notFound, unavailable, unprocessable } from '../../shared/errors.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import type { Actor } from '../auth/model.js'
import type { EnvironmentView } from '../environments/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import {
  compileGrayRulesWire,
  countEffectiveGrayRules,
  GRAY_COMPILER_REVISION,
  GRAY_MATCH_DATA_ID,
  GRAY_MATCH_GROUP,
  parseGrayRulesWire,
} from './model.js'
import type {
  CreateGrayReleaseInput,
  GrayLiveView,
  GrayReleaseView,
  QueueGrayPublicationResult,
  RollbackGrayReleaseInput,
} from './release-model.js'
import { GrayRuleReleaseRepository, type PreparedGrayReleasePlan } from './release-repository.js'
import { GrayRuleDraftRepository } from './repository.js'
import { validateGrayRules } from './validate.js'

export interface GrayRuleReleaseService {
  create(actor: Actor, input: CreateGrayReleaseInput, requestId: string): Promise<GrayReleaseView>
  rollback(
    actor: Actor,
    releaseId: string,
    input: RollbackGrayReleaseInput,
    requestId: string,
  ): Promise<GrayReleaseView>
  list(actor: Actor): Promise<{ items: readonly GrayReleaseView[] }>
  get(actor: Actor, releaseId: string): Promise<GrayReleaseView>
  queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueGrayPublicationResult>
  currentLive(actor: Actor): Promise<GrayLiveView>
}

export class DefaultGrayRuleReleaseService implements GrayRuleReleaseService {
  readonly #drafts: GrayRuleDraftRepository
  readonly #releases: GrayRuleReleaseRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator
  readonly #nacos: NacosClient

  constructor(
    drafts: GrayRuleDraftRepository,
    releases: GrayRuleReleaseRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
    nacos: NacosClient,
  ) {
    this.#drafts = drafts
    this.#releases = releases
    this.#environments = environments
    this.#validator = validator
    this.#nacos = nacos
  }

  async create(
    actor: Actor,
    input: CreateGrayReleaseInput,
    requestId: string,
  ): Promise<GrayReleaseView> {
    const workspace = await this.#requireWorkspace(actor)
    await this.#requirePublisher(actor, workspace.id)
    if (!this.#validator.available || !this.#validator.revision) {
      throw unavailable(
        'NATIVE_VALIDATOR_UNCONFIGURED',
        'Native Validator is required before creating a gray rules release',
      )
    }
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }

    const draft = await this.#drafts.findByEnvironmentInternalId(workspace.internalId)
    if (!draft) throw notFound('Gray rules draft')
    const issues = validateGrayRules(draft.document, this.#validator.limits?.grayRules)
    if (issues.length > 0) {
      throw unprocessable(
        'GRAY_RULES_NOT_PUBLISHABLE',
        'The gray rules draft is invalid',
        issues.map((issue) => ({ path: issue.field, code: issue.code, message: issue.message })),
      )
    }

    const plan: PreparedGrayReleasePlan = {
      wirePayload: compileGrayRulesWire(draft.document),
      ruleCount: countEffectiveGrayRules(draft.document),
      entries: draft.document.rules.map((rule) => rule.entry),
    }

    await this.#validatePayload(plan.wirePayload, requestId, 'GRAY_RULES_NOT_PUBLISHABLE')
    const base = await this.#readBase(workspace)
    const description = input.description?.trim() ?? ''

    return this.#releases.create(
      actor,
      workspace.internalId,
      plan,
      base,
      {
        idempotencyKey: input.idempotencyKey,
        requestSha256: sha256(
          canonicalJson({
            kind: 'gray_rules',
            description,
            wireSha256: sha256(plan.wirePayload).toString('hex'),
          }),
        ),
        compilerRevision: GRAY_COMPILER_REVISION,
        validatorContractVersion: this.#validator.contractVersion,
        validatorRevision: this.#validator.revision,
        description,
      },
      requestId,
    )
  }

  async rollback(
    actor: Actor,
    releaseId: string,
    input: RollbackGrayReleaseInput,
    requestId: string,
  ): Promise<GrayReleaseView> {
    const workspace = await this.#requireWorkspace(actor)
    await this.#requirePublisher(actor, workspace.id)
    if (!this.#validator.available || !this.#validator.revision) {
      throw unavailable(
        'NATIVE_VALIDATOR_UNCONFIGURED',
        'Native Validator is required before rolling back a gray rules release',
      )
    }
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }

    const source = await this.#releases.loadRollbackSource(workspace.internalId, releaseId)
    if (!source) throw notFound('Gray release')

    await this.#validatePayload(source.plan.wirePayload, requestId, 'GRAY_ROLLBACK_NOT_PUBLISHABLE')
    const base = await this.#readBase(workspace)

    return this.#releases.create(
      actor,
      workspace.internalId,
      source.plan,
      base,
      {
        idempotencyKey: input.idempotencyKey,
        requestSha256: sha256(
          canonicalJson({ kind: 'gray_rules_rollback', sourceReleaseId: releaseId }),
        ),
        compilerRevision: source.compilerRevision,
        validatorContractVersion: this.#validator.contractVersion,
        validatorRevision: this.#validator.revision,
        description: `rollback of gray rules #${source.sequence}`,
        rollbackOfSequence: source.sequence,
      },
      requestId,
    )
  }

  async list(actor: Actor): Promise<{ items: readonly GrayReleaseView[] }> {
    const workspace = await this.#requireWorkspace(actor)
    return { items: await this.#releases.list(workspace.internalId) }
  }

  async get(actor: Actor, releaseId: string): Promise<GrayReleaseView> {
    const workspace = await this.#requireWorkspace(actor)
    const release = await this.#releases.find(releaseId, workspace.internalId)
    if (!release) throw notFound('Gray release')
    return release
  }

  async queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueGrayPublicationResult> {
    const workspace = await this.#requireWorkspace(actor)
    await this.#requirePublisher(actor, workspace.id)
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    const release = await this.#releases.find(releaseId, workspace.internalId)
    if (!release) throw notFound('Gray release')
    return this.#releases.queuePublication(actor, releaseId, idempotencyKey, requestId)
  }

  async currentLive(actor: Actor): Promise<GrayLiveView> {
    const workspace = await this.#requireWorkspace(actor)
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    const target: NacosTarget = {
      endpoint: workspace.nacos.endpoint,
      namespace: workspace.nacos.namespace,
      tenant: workspace.nacos.tenant,
      credentialConfigured: workspace.nacos.credentialConfigured,
    }
    let liveValue: NacosResourceValue
    try {
      liveValue = await this.#nacos.read(target, GRAY_MATCH_DATA_ID, GRAY_MATCH_GROUP)
    } catch {
      throw unavailable(
        'NACOS_READ_FAILED',
        'The gray-match configuration could not be read from rnacos',
      )
    }
    if (!liveValue.exists || liveValue.content === null) {
      return {
        exists: false,
        dataId: GRAY_MATCH_DATA_ID,
        group: GRAY_MATCH_GROUP,
        sha256: null,
        md5: null,
        content: null,
        document: null,
        parseError: null,
      }
    }
    const parsed = parseGrayRulesWire(liveValue.content)
    return {
      exists: true,
      dataId: GRAY_MATCH_DATA_ID,
      group: GRAY_MATCH_GROUP,
      sha256: liveValue.sha256,
      md5: liveValue.md5,
      content: liveValue.content,
      document: parsed.document,
      parseError: parsed.parseError,
    }
  }

  async #validatePayload(wirePayload: string, requestId: string, errorCode: string): Promise<void> {
    const result = await this.#validator.validate({
      requestId,
      kind: 'gray_rules',
      project: '',
      payload: Buffer.from(wirePayload, 'utf8'),
    })
    if (!result.valid) {
      throw unprocessable(
        errorCode,
        errorCode === 'GRAY_ROLLBACK_NOT_PUBLISHABLE'
          ? 'The historical gray rules payload failed native validation'
          : 'The compiled gray rules payload failed native validation',
        result.errors.map((error) => ({
          path: error.field ?? '',
          code: error.code,
          message: error.message,
        })),
      )
    }
  }

  async #readBase(
    workspace: EnvironmentView & { internalId: string },
  ): Promise<NacosResourceValue> {
    const target: NacosTarget = {
      endpoint: workspace.nacos.endpoint,
      namespace: workspace.nacos.namespace,
      tenant: workspace.nacos.tenant,
      credentialConfigured: workspace.nacos.credentialConfigured,
    }
    try {
      return await this.#nacos.read(target, GRAY_MATCH_DATA_ID, GRAY_MATCH_GROUP)
    } catch {
      throw unavailable(
        'NACOS_PREFLIGHT_FAILED',
        'The gray-match resource could not be read before creating the release',
      )
    }
  }

  async #requireWorkspace(actor: Actor): Promise<EnvironmentView & { internalId: string }> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) throw notFound('Deployment workspace')
    const internalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!internalId) throw notFound('Deployment workspace')
    return { ...environment, internalId }
  }

  async #requirePublisher(actor: Actor, environmentPublicId: string): Promise<void> {
    const role = await this.#environments.role(actor, environmentPublicId)
    if (!role) throw notFound('Deployment workspace')
    if (!['admin', 'publisher'].includes(role)) throw forbidden()
  }
}

export class UnavailableGrayRuleReleaseService implements GrayRuleReleaseService {
  async create(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
  async rollback(): Promise<never> {
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
  async currentLive(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
