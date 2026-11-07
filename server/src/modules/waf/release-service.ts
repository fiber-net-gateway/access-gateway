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
import { emptyEdgeFunctionsDraft, type EdgeFunctionsDraft } from './edge-function-model.js'
import { EdgeFunctionDraftRepository } from './edge-function-repository.js'
import { WAF_FUNCTION_NAME, WAF_MOUNT_PRIORITY, type WafPolicy } from './model.js'
import {
  EDGE_FUNCTIONS_DATA_ID,
  EDGE_FUNCTIONS_GROUP,
  edgeFunctionScriptDataId,
  isScriptDataIdSafeName,
  type CreateWafReleaseInput,
  type QueueWafPublicationResult,
  type RollbackWafReleaseInput,
  type WafEdgeFunctionIndexEntryView,
  type WafEdgeFunctionsView,
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
  rollback(
    actor: Actor,
    releaseId: string,
    input: RollbackWafReleaseInput,
    requestId: string,
  ): Promise<WafReleaseView>
  list(actor: Actor): Promise<{ items: readonly WafReleaseView[] }>
  get(actor: Actor, releaseId: string): Promise<WafReleaseView>
  queuePublication(
    actor: Actor,
    releaseId: string,
    idempotencyKey: string,
    requestId: string,
  ): Promise<QueueWafPublicationResult>
  /** Live read-only snapshot of the edge-function index + scripts in rnacos. */
  currentEdgeFunctions(actor: Actor): Promise<WafEdgeFunctionsView>
}

// The index version is allocated inside the repository transaction, so the
// validator sees a structurally identical representative index (same functions
// array, placeholder version); only the embedded integer differs, and its
// range is enforced at allocation time.
const representativeIndexVersion = 1

// The published index carries at most maxFunctions entries; the live view only
// ever fetches a handful of scripts and never trusts the stored index to be
// console-authored.
const liveScriptReadLimit = 16

function parseLiveIndex(content: string): {
  version: number | null
  functions: WafEdgeFunctionIndexEntryView[]
  parseError: string | null
} {
  let decoded: unknown
  try {
    decoded = JSON.parse(content) as unknown
  } catch (error) {
    return {
      version: null,
      functions: [],
      parseError: `index is not valid JSON: ${error instanceof Error ? error.message : 'unknown error'}`,
    }
  }
  if (typeof decoded !== 'object' || decoded === null || Array.isArray(decoded)) {
    return { version: null, functions: [], parseError: 'index is not a JSON object' }
  }
  const record = decoded as Record<string, unknown>
  if (!Array.isArray(record.functions)) {
    return {
      version: typeof record.version === 'number' ? record.version : null,
      functions: [],
      parseError: 'index has no functions array',
    }
  }
  const functions = record.functions.flatMap((entry): WafEdgeFunctionIndexEntryView[] => {
    if (typeof entry !== 'object' || entry === null) return []
    const functionRecord = entry as Record<string, unknown>
    if (typeof functionRecord.name !== 'string') return []
    const mounts = Array.isArray(functionRecord.mounts)
      ? functionRecord.mounts.flatMap((mount): WafEdgeFunctionIndexEntryView['mounts'] => {
          if (typeof mount !== 'object' || mount === null) return []
          const mountRecord = mount as Record<string, unknown>
          const projects =
            typeof mountRecord.projects === 'object' && mountRecord.projects !== null
              ? (mountRecord.projects as Record<string, unknown>)
              : {}
          const stringList = (value: unknown): string[] =>
            Array.isArray(value)
              ? value.filter((item): item is string => typeof item === 'string')
              : []
          return [
            {
              projects: {
                include: stringList(projects.include),
                exclude: stringList(projects.exclude),
              },
              priority: typeof mountRecord.priority === 'number' ? mountRecord.priority : null,
              ratio: typeof mountRecord.ratio === 'number' ? mountRecord.ratio : null,
            },
          ]
        })
      : []
    return [
      {
        name: functionRecord.name,
        sha256: typeof functionRecord.sha256 === 'string' ? functionRecord.sha256 : null,
        phase:
          functionRecord.phase === 'request_head' || functionRecord.phase === 'request_body'
            ? functionRecord.phase
            : null,
        mounts,
      },
    ]
  })
  return {
    version: typeof record.version === 'number' ? record.version : null,
    functions,
    parseError: null,
  }
}

function planFromPolicy(
  policy: WafPolicy,
  functionsDraft: EdgeFunctionsDraft,
): PreparedWafReleasePlan {
  const compilation = compileWafPolicy(policy)
  const wafMounted = compilation.kind !== 'unmount'
  const [selector] = compilation.kind === 'mounted' ? compilation.mounts : []
  const wafEntry: WafWireIndexFunctionEntry | null = wafMounted
    ? {
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
      }
    : null

  const handwritten = functionsDraft.functions.filter((fn) => fn.enabled)
  const entries: WafWireIndexFunctionEntry[] = [
    ...(wafEntry ? [wafEntry] : []),
    ...handwritten.map((fn) => ({
      name: fn.name,
      sha256: sha256(fn.script).toString('hex'),
      // Omit the wire default (request_head) so canonical index bytes stay
      // minimal; a body-phase function only buffers for opted-in routes.
      ...(fn.phase === 'request_body' ? { phase: 'request_body' as const } : {}),
      mounts: fn.mounts.map((mount) => ({
        projects: {
          include: [...mount.projects.include],
          exclude: [...(mount.projects.exclude ?? [])],
        },
        priority: mount.priority ?? 0,
        // Omit the wire default (10000) so canonical index bytes stay minimal.
        ...(mount.ratio !== undefined && mount.ratio !== 10_000 ? { ratio: mount.ratio } : {}),
      })),
    })),
  ]
  // Deterministic declaration order: ascending lowest-mount priority, then
  // name — the codec executes mounts in priority order and breaks ties by
  // declaration, so the published order must not depend on draft array order.
  const minPriority = (entry: WafWireIndexFunctionEntry): number =>
    Math.min(...entry.mounts.map((mount) => mount.priority))
  entries.sort((a, b) => minPriority(a) - minPriority(b) || (a.name < b.name ? -1 : 1))

  const scripts: PreparedWafReleasePlan['scripts'] = [
    ...(wafMounted && compilation.kind === 'mounted'
      ? [{ name: compilation.functionName, script: compilation.script }]
      : []),
    ...handwritten.map((fn) => ({
      name: fn.name,
      script: fn.script,
      ...(fn.phase === 'request_body' ? { phase: 'request_body' as const } : {}),
    })),
  ]

  return {
    mode: wafMounted ? 'mounted' : 'unmount',
    functionName: wafEntry?.name ?? WAF_FUNCTION_NAME,
    script: wafMounted && compilation.kind === 'mounted' ? compilation.script : null,
    scriptSha256: wafEntry?.sha256 ?? null,
    ruleCount: policy.rules.length,
    functions: entries,
    scripts,
  }
}

export class DefaultWafReleaseService implements WafReleaseService {
  readonly #drafts: WafPolicyDraftRepository
  readonly #functionDrafts: EdgeFunctionDraftRepository
  readonly #releases: WafReleaseRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator
  readonly #nacos: NacosClient

  constructor(
    drafts: WafPolicyDraftRepository,
    functionDrafts: EdgeFunctionDraftRepository,
    releases: WafReleaseRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
    nacos: NacosClient,
  ) {
    this.#drafts = drafts
    this.#functionDrafts = functionDrafts
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
    const functionsDraftView = await this.#functionDrafts.findByEnvironmentInternalId(
      workspace.internalId,
    )
    const functionsDraft = functionsDraftView?.draft ?? emptyEdgeFunctionsDraft()
    const plan = planFromPolicy(draft.policy, functionsDraft)
    this.#checkCardinalityLimits(plan)

    await this.#validatePlan(plan, requestId, 'WAF_COMPILATION_NOT_PUBLISHABLE')
    const bases = await this.#readBases(
      workspace,
      plan.scripts.map((entry) => entry.name),
    )

    return this.#releases.create(
      actor,
      workspace.internalId,
      plan,
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

  // Rollback republishes a historical release's immutable payload documents as
  // a NEW release: the plan is rebuilt from the stored payloads (digests
  // verified by the repository), re-validated through today's native validator
  // fail-closed, and persisted with a freshly allocated index version. Release
  // history is never rewritten; the current WAF draft is not touched.
  async rollback(
    actor: Actor,
    releaseId: string,
    input: RollbackWafReleaseInput,
    requestId: string,
  ): Promise<WafReleaseView> {
    const workspace = await this.#requireWorkspace(actor)
    await this.#requirePublisher(actor, workspace.id)
    if (!this.#validator.available || !this.#validator.revision || !this.#validator.limits) {
      throw unavailable(
        'NATIVE_VALIDATOR_UNCONFIGURED',
        'Native Validator is required before rolling back a WAF release',
      )
    }
    if (!this.#nacos.available) {
      throw unavailable('PUBLICATION_UNCONFIGURED', 'Nacos publication is not configured')
    }

    const source = await this.#releases.loadRollbackSource(workspace.internalId, releaseId)
    if (!source) throw notFound('WAF release')
    this.#checkCardinalityLimits(source.plan)
    await this.#validatePlan(source.plan, requestId, 'WAF_ROLLBACK_NOT_PUBLISHABLE')
    const bases = await this.#readBases(
      workspace,
      source.plan.scripts.map((entry) => entry.name),
    )

    return this.#releases.create(
      actor,
      workspace.internalId,
      source.plan,
      bases,
      {
        idempotencyKey: input.idempotencyKey,
        requestSha256: sha256(
          canonicalJson({ kind: 'waf_edge_functions_rollback', sourceReleaseId: releaseId }),
        ),
        // The content was produced by the historical generator revision; keep
        // that provenance instead of stamping today's revision on old bytes.
        generatorRevision: source.generatorRevision || WAF_GENERATOR_REVISION,
        validatorContractVersion: this.#validator.contractVersion,
        validatorRevision: this.#validator.revision,
        description: `rollback of WAF edge functions #${source.sequence}`,
        rollbackOfSequence: source.sequence,
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

  // Live snapshot for the read-only 边缘函数页 (§4.4): exactly the bytes the
  // data plane sees in rnacos right now, distinct from release history. The
  // index is parsed defensively — an unparseable or foreign index is reported
  // as a parse error instead of failing the whole view.
  async currentEdgeFunctions(actor: Actor): Promise<WafEdgeFunctionsView> {
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
    const emptyIndex: WafEdgeFunctionsView['index'] = {
      exists: false,
      sha256: null,
      md5: null,
      version: null,
      functions: [],
      parseError: null,
    }
    let indexValue: NacosResourceValue
    try {
      indexValue = await this.#nacos.read(target, EDGE_FUNCTIONS_DATA_ID, EDGE_FUNCTIONS_GROUP)
    } catch {
      throw unavailable(
        'NACOS_READ_FAILED',
        'The edge-function index could not be read from rnacos',
      )
    }
    if (!indexValue.exists || indexValue.content === null) {
      return { index: emptyIndex, scripts: [] }
    }

    const parsed = parseLiveIndex(indexValue.content)
    const names = [
      ...new Set(
        parsed.functions.map((entry) => entry.name).filter((name) => isScriptDataIdSafeName(name)),
      ),
    ].slice(0, liveScriptReadLimit)
    const scripts: {
      name: string
      exists: boolean
      sha256: string | null
      content: string | null
    }[] = []
    for (const name of names) {
      let script: NacosResourceValue
      try {
        script = await this.#nacos.read(
          target,
          edgeFunctionScriptDataId(name),
          EDGE_FUNCTIONS_GROUP,
        )
      } catch {
        throw unavailable(
          'NACOS_READ_FAILED',
          `The script for edge function "${name}" could not be read from rnacos`,
        )
      }
      scripts.push({
        name,
        exists: script.exists,
        sha256: script.exists ? script.sha256 : null,
        content: script.exists ? script.content : null,
      })
    }
    return {
      index: {
        exists: true,
        sha256: indexValue.sha256,
        md5: indexValue.md5,
        version: parsed.version,
        functions: parsed.functions,
        parseError: parsed.parseError,
      },
      scripts,
    }
  }

  // Fail-closed: every script payload plus the index must pass the native
  // validator before a Release can be prepared (§4.1 全环境校验). Rollback
  // reuses this with its own error code so operators can tell a stale
  // historical payload from a bad draft compilation.
  async #validatePlan(
    plan: PreparedWafReleasePlan,
    requestId: string,
    errorCode: string,
  ): Promise<void> {
    // Each script compiles in its own phase: $req.body resolves only for
    // request_body, so the head/body distinction must reach the validator.
    const validations = plan.scripts.map((entry) =>
      this.#validator.validate({
        requestId,
        kind: 'edge_function_script',
        project: '',
        payload: Buffer.from(entry.script, 'utf8'),
        ...(entry.phase !== undefined ? { phase: entry.phase } : {}),
      }),
    )
    const representativeIndex = canonicalJson({
      version: representativeIndexVersion,
      functions: plan.functions,
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
        errorCode,
        errorCode === 'WAF_ROLLBACK_NOT_PUBLISHABLE'
          ? 'The historical WAF artifacts failed native validation'
          : 'The compiled WAF artifacts failed native validation',
        errors,
      )
    }
  }

  // Pre-flight the cardinality ceilings the native codec enforces so the
  // operator sees a precise error instead of a validator dump. The validator
  // re-checks everything; these are the checks it cannot attribute cleanly.
  #checkCardinalityLimits(plan: PreparedWafReleasePlan): void {
    const limits = this.#validator.limits?.edgeFunctions
    if (!limits) return
    if (plan.functions.length > limits.maxFunctions) {
      throw unprocessable(
        'EDGE_FUNCTION_COUNT_EXCEEDED',
        `A release may mount at most ${limits.maxFunctions} edge functions`,
      )
    }
    const totalScriptBytes = plan.scripts.reduce(
      (total, entry) => total + Buffer.byteLength(entry.script, 'utf8'),
      0,
    )
    if (totalScriptBytes > limits.maxTotalScriptBytes) {
      throw unprocessable(
        'EDGE_FUNCTION_SCRIPT_BUDGET_EXCEEDED',
        `The release scripts total ${totalScriptBytes} bytes, above the ${limits.maxTotalScriptBytes}-byte snapshot budget`,
      )
    }
  }

  async #readBases(
    workspace: EnvironmentView & { internalId: string },
    scriptNames: readonly string[],
  ): Promise<{
    index: NacosResourceValue
    scripts: { name: string; base: NacosResourceValue }[]
  }> {
    const target: NacosTarget = {
      endpoint: workspace.nacos.endpoint,
      namespace: workspace.nacos.namespace,
      tenant: workspace.nacos.tenant,
      credentialConfigured: workspace.nacos.credentialConfigured,
    }
    try {
      const index = await this.#nacos.read(target, EDGE_FUNCTIONS_DATA_ID, EDGE_FUNCTIONS_GROUP)
      const scripts: { name: string; base: NacosResourceValue }[] = []
      for (const name of scriptNames) {
        scripts.push({
          name,
          base: await this.#nacos.read(
            target,
            edgeFunctionScriptDataId(name),
            EDGE_FUNCTIONS_GROUP,
          ),
        })
      }
      return { index, scripts }
    } catch {
      throw unavailable(
        'NACOS_PREFLIGHT_FAILED',
        'The edge-function resources could not be read before creating the WAF release',
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

export class UnavailableWafReleaseService implements WafReleaseService {
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
  async currentEdgeFunctions(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
