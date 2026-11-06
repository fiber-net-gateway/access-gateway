// WAF policy service: staged validation → draft persistence → compile
// preview. Field syntax and cross-resource checks (validate.ts) gate saves;
// the native validator compile of the generated script (kind
// 'edge_function_script') is the fail-closed authority for dialect and CIDR
// semantics and runs on every compile preview ahead of publication wiring.

import type {
  NativeValidationError,
  NativeValidator,
} from '../../integrations/native-validator/model.js'
import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { WafCompilation } from './generator.js'
import { compileWafPolicy } from './generator.js'
import type { WafPolicy } from './model.js'
import { WafPolicyDraftRepository, type WafPolicyDraftView } from './repository.js'
import { validateWafPolicy, type WafValidationIssue } from './validate.js'

export type { WafPolicyDraftView } from './repository.js'

export interface SaveWafPolicyInput {
  lockVersion: string
  policy: unknown
}

export interface WafCompilePreviewView {
  /** False when the policy has field-level issues; compilation is then null. */
  valid: boolean
  issues: readonly WafValidationIssue[]
  compilation: WafCompilation | null
  /** Null when there is no script to compile (unmount candidate). */
  scriptValid: boolean | null
  scriptErrors: readonly NativeValidationError[]
  validator: { contractVersion: number; revision: string } | null
}

export interface WafService {
  getPolicyDraft(actor: Actor): Promise<WafPolicyDraftView>
  createPolicyDraft(actor: Actor, requestId: string): Promise<WafPolicyDraftView>
  savePolicyDraft(
    actor: Actor,
    input: SaveWafPolicyInput,
    requestId: string,
  ): Promise<WafPolicyDraftView>
  compilePreview(
    actor: Actor,
    policy: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<WafCompilePreviewView>
}

function parseWafPolicy(value: unknown): WafPolicy {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw badRequest('INVALID_WAF_POLICY', 'WAF policy must be an object', [
      { path: 'policy', code: 'invalid_field', message: 'WAF policy must be an object' },
    ])
  }
  const issues = validateWafPolicy(value as WafPolicy)
  if (issues.length > 0) {
    throw badRequest(
      'INVALID_WAF_POLICY',
      'The WAF policy contains invalid fields',
      issues.map((issue) => ({ path: issue.field, code: issue.code, message: issue.message })),
    )
  }
  return value as WafPolicy
}

function parseLockVersion(value: string): string {
  if (!/^(0|[1-9][0-9]*)$/u.test(value)) {
    throw badRequest('INVALID_LOCK_VERSION', 'lockVersion must be an unsigned integer string')
  }
  return value
}

export class DefaultWafService implements WafService {
  readonly #drafts: WafPolicyDraftRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator

  constructor(
    drafts: WafPolicyDraftRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
  ) {
    this.#drafts = drafts
    this.#environments = environments
    this.#validator = validator
  }

  async getPolicyDraft(actor: Actor): Promise<WafPolicyDraftView> {
    const environmentInternalId = await this.#requireEnvironment(actor)
    const draft = await this.#drafts.findByEnvironmentInternalId(environmentInternalId)
    if (!draft) {
      throw notFound('WAF policy draft')
    }
    return draft
  }

  async createPolicyDraft(actor: Actor, requestId: string): Promise<WafPolicyDraftView> {
    const environmentInternalId = await this.#requireEditorEnvironment(actor)
    return this.#drafts.getOrCreate(actor, environmentInternalId, requestId)
  }

  async savePolicyDraft(
    actor: Actor,
    input: SaveWafPolicyInput,
    requestId: string,
  ): Promise<WafPolicyDraftView> {
    const environmentInternalId = await this.#requireEditorEnvironment(actor)
    const policy = parseWafPolicy(input.policy)
    return this.#drafts.save(
      actor,
      environmentInternalId,
      parseLockVersion(input.lockVersion),
      policy,
      requestId,
    )
  }

  async compilePreview(
    actor: Actor,
    policy: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<WafCompilePreviewView> {
    await this.#requireEditorEnvironment(actor)
    if (typeof policy !== 'object' || policy === null || Array.isArray(policy)) {
      return {
        valid: false,
        issues: [
          { field: 'policy', code: 'invalid_field', message: 'WAF policy must be an object' },
        ],
        compilation: null,
        scriptValid: null,
        scriptErrors: [],
        validator: null,
      }
    }
    const issues = validateWafPolicy(policy as WafPolicy)
    if (issues.length > 0) {
      return {
        valid: false,
        issues,
        compilation: null,
        scriptValid: null,
        scriptErrors: [],
        validator: null,
      }
    }
    const compilation = compileWafPolicy(policy as WafPolicy)
    if (compilation.kind === 'unmount') {
      return {
        valid: true,
        issues: [],
        compilation,
        scriptValid: null,
        scriptErrors: [],
        validator: null,
      }
    }
    const native = await this.#validator.validate(
      {
        requestId,
        kind: 'edge_function_script',
        project: '',
        payload: Buffer.from(compilation.script, 'utf8'),
      },
      signal,
    )
    return {
      valid: true,
      issues: [],
      compilation,
      scriptValid: native.valid,
      scriptErrors: native.errors,
      validator: { contractVersion: native.contractVersion, revision: native.validatorRevision },
    }
  }

  async #requireWorkspace(actor: Actor): Promise<{ internalId: string; publicId: string }> {
    const environment = await this.#environments.findWorkspace(actor)
    if (!environment) {
      throw notFound('Deployment workspace')
    }
    const internalId = await this.#environments.internalIdForActor(actor, environment.id)
    if (!internalId) {
      throw notFound('Deployment workspace')
    }
    return { internalId, publicId: environment.id }
  }

  async #requireEnvironment(actor: Actor): Promise<string> {
    return (await this.#requireWorkspace(actor)).internalId
  }

  async #requireEditorEnvironment(actor: Actor): Promise<string> {
    const workspace = await this.#requireWorkspace(actor)
    const role = await this.#environments.role(actor, workspace.publicId)
    if (!role) {
      throw notFound('Environment')
    }
    if (!['admin', 'maintainer'].includes(role)) {
      throw forbidden()
    }
    return workspace.internalId
  }
}

export class UnavailableWafService implements WafService {
  async getPolicyDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async createPolicyDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async savePolicyDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async compilePreview(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
