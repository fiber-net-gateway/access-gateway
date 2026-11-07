// WAF release service: compiles the current WAF policy draft into the
// edge-function wire artifacts (script + function index), fail-closes on the
// native validator for both payloads, and persists an immutable release whose
// resources the publication worker writes scripts-first-index-last
// (§4.3). Activation stays 'unknown' until the edge-function evidence wiring
// lands; rnacos write/readback is never treated as activation evidence.

import type {
  NacosClient,
  NacosResourceValue,
  NacosTarget,
} from '../../integrations/nacos/model.js'
import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { forbidden, notFound, unavailable, unprocessable } from '../../shared/errors.js'
import { canonicalJson, sha256 } from '../../shared/json.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { EnvironmentView } from '../environments/model.js'
import { compileWafPolicy } from './generator.js'
import { WAF_FUNCTION_NAME, WAF_MOUNT_PRIORITY, type WafPolicy } from './model.js'
import {
  EDGE_FUNCTIONS_DATA_ID,
  EDGE_FUNCTIONS_GROUP,
  edgeFunctionScriptDataId,
  type CreateWafReleaseInput,
  type QueueWafPublicationResult,
  type WafReleaseView,
} from './release-model.js'
import {
  WafReleaseRepository,
  type PreparedWafReleasePlan,
  type WafWireIndexFunctionEntry,
} from './release-repository.js'
import { WafPolicyDraftRepository } from './repository.js'
import { validateWafPolicy } from './validate.js'

/** Identity of the generator output; bump when generation semantics change. */
export const WAF_GENERATOR_REVISION = 'waf-generator-v1'

export interface WafReleaseService {
  create(actor: Actor, input: CreateWafReleaseInput, requestId: string): Promise<WafReleaseView>
  list(actor: Actor): Promise<{ items: readonly WafReleaseView[] }>
  get(actor: Actor, releaseId: string): Promise<WafReleaseView>
  queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueWafPublicationResult>
}

// The index version is allocated inside the repository transaction, so the
// validator sees a structurally identical representative index (same functions
// array, placeholder version); only the embedded integer differs, and its
// range is enforced at allocation time.
const representativeIndexVersion = 1

function planFromPolicy(policy: WafPolicy): {
  plan: Omit<PreparedWafReleasePlan, 'functions'>
  functions: readonly WafWireIndexFunctionEntry[]
} {
  const compilation = compileWafPolicy(policy)
  if (compilation.kind === 'unmount') {
    return {
      plan: {
        mode: 'unmount',
        functionName: WAF_FUNCTION_NAME,
        script: null,
        scriptSha256: null,
        ruleCount: policy.rules.length,
      },
      functions: [],
    }
  }
  const [selector] = compilation.mounts
  return {
    plan: {
      mode: 'mounted',
      functionName: compilation.functionName,
      script: compilation.script,
      scriptSha256: compilation.scriptSha256,
      ruleCount: policy.rules.length,
    },
    functions: [
      {
        name: compilation.functionName,
        sha256: compilation.scriptSha256,
        mounts: [
          {
            projects: {
              include: [...(selector?.include ?? [])],
              exclude: [...(selector?.exclude ?? [])],
            },
            priority: compilation.mountPriority ?? WAF_MOUNT_PRIORITY,
          },
        ],
      },
    ],
  }
}

export class DefaultWafReleaseService implements WafReleaseService {
  readonly #drafts: WafPolicyDraftRepository
  readonly #releases: WafReleaseRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator
  readonly #nacos: NacosClient

  constructor(
    drafts: WafPolicyDraftRepository,
    releases: WafReleaseRepository,
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
    input: CreateWafReleaseInput,
    requestId: string,
  ): Promise<WafReleaseView> {
    const workspace = await this.#requireWorkspace(actor)
    await this.#requirePublisher(actor, workspace.id)
    if (!this.#validator.available || !this.#validator.revision || !this.#validator.limits) {
      throw unavailable(
        'NATIVE_VALIDATOR_UNCONFIGURED',
        'Native Validator is required before creating a WAF release',
      )
    }
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }

    const draft = await this.#drafts.findByEnvironmentInternalId(workspace.internalId)
    if (!draft) throw notFound('WAF policy draft')
    const issues = validateWafPolicy(draft.policy)
    if (issues.length > 0) {
      throw unprocessable(
        'WAF_POLICY_NOT_PUBLISHABLE',
        'The WAF policy draft is invalid',
        issues.map((issue) => ({ path: issue.field, code: issue.code, message: issue.message })),
      )
    }

    const { plan, functions } = planFromPolicy(draft.policy)
    // Fail-closed: both wire payloads must compile through the native
    // validator before a Release can be prepared (§4.1 全环境校验).
    const validations = []
    if (plan.mode === 'mounted' && plan.script !== null) {
      validations.push(
        this.#validator.validate({
          requestId,
          kind: 'edge_function_script',
          project: '',
          payload: Buffer.from(plan.script, 'utf8'),
        }),
      )
    }
    const representativeIndex = canonicalJson({
      version: representativeIndexVersion,
      functions,
    })
    validations.push(
      this.#validator.validate({
        requestId,
        kind: 'edge_function_index',
        project: '',
        payload: Buffer.from(representativeIndex, 'utf8'),
      }),
    )
    const results = await Promise.all(validations)
    const errors = results.flatMap((result) =>
      result.valid
        ? []
        : result.errors.map((error) => ({
            path: error.field ?? '',
            code: error.code,
            message: error.message,
          })),
    )
    if (errors.length > 0) {
      throw unprocessable(
        'WAF_COMPILATION_NOT_PUBLISHABLE',
        'The compiled WAF artifacts failed native validation',
        errors,
      )
    }

    const target: NacosTarget = {
      endpoint: workspace.nacos.endpoint,
      namespace: workspace.nacos.namespace,
      tenant: workspace.nacos.tenant,
      credentialConfigured: workspace.nacos.credentialConfigured,
    }
    const scriptDataId = edgeFunctionScriptDataId(plan.functionName)
    let bases: { index: NacosResourceValue; script: NacosResourceValue | null }
    try {
      bases = {
        index: await this.#nacos.read(target, EDGE_FUNCTIONS_DATA_ID, EDGE_FUNCTIONS_GROUP),
        script:
          plan.mode === 'mounted'
            ? await this.#nacos.read(target, scriptDataId, EDGE_FUNCTIONS_GROUP)
            : null,
      }
    } catch {
      throw unavailable(
        'NACOS_PREFLIGHT_FAILED',
        'The edge-function resources could not be read before creating the WAF release',
      )
    }

    return this.#releases.create(
      actor,
      workspace.internalId,
      { ...plan, functions },
      bases,
      {
        idempotencyKey: input.idempotencyKey,
        requestSha256: sha256(canonicalJson({ kind: 'waf_edge_functions' })),
        generatorRevision: WAF_GENERATOR_REVISION,
        validatorContractVersion: this.#validator.contractVersion,
        validatorRevision: this.#validator.revision,
      },
      requestId,
    )
  }

  async list(actor: Actor): Promise<{ items: readonly WafReleaseView[] }> {
    const workspace = await this.#requireWorkspace(actor)
    return { items: await this.#releases.list(workspace.internalId) }
  }

  async get(actor: Actor, releaseId: string): Promise<WafReleaseView> {
    const workspace = await this.#requireWorkspace(actor)
    const release = await this.#releases.find(releaseId, workspace.internalId)
    if (!release) throw notFound('WAF release')
    return release
  }

  async queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueWafPublicationResult> {
    const workspace = await this.#requireWorkspace(actor)
    await this.#requirePublisher(actor, workspace.id)
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }
    const release = await this.#releases.find(releaseId, workspace.internalId)
    if (!release) throw notFound('WAF release')
    return this.#releases.queuePublication(actor, releaseId, idempotencyKey, requestId)
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

export class UnavailableWafReleaseService implements WafReleaseService {
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
