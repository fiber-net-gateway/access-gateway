import type {
  NativeValidationError,
  NativeValidator,
} from '../../integrations/native-validator/model.js'
import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import { sha256 } from '../../shared/json.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import {
  compileGrayRulesWire,
  countEffectiveGrayRules,
  GRAY_MATCH_DATA_ID,
  GRAY_MATCH_GROUP,
  type GrayRulesDocument,
} from './model.js'
import { GrayRuleDraftRepository, type GrayRulesDraftView } from './repository.js'
import { validateGrayRules, type GrayValidationIssue } from './validate.js'

export type { GrayRulesDraftView } from './repository.js'

export interface SaveGrayRulesInput {
  lockVersion: string
  document: unknown
}

export interface GrayValidationPreviewView {
  valid: boolean
  issues: readonly GrayValidationIssue[]
  wirePreview: string | null
  wireSha256: string | null
  dataId: string
  group: string
  ruleCount: number
  nativeValid: boolean | null
  nativeErrors: readonly NativeValidationError[]
  validator: { contractVersion: number; revision: string } | null
}

export interface GrayRuleService {
  getDraft(actor: Actor): Promise<GrayRulesDraftView>
  createDraft(actor: Actor, requestId: string): Promise<GrayRulesDraftView>
  saveDraft(actor: Actor, input: SaveGrayRulesInput, requestId: string): Promise<GrayRulesDraftView>
  validatePreview(
    actor: Actor,
    document: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<GrayValidationPreviewView>
}

function parseLockVersion(value: string): string {
  if (!/^(0|[1-9][0-9]*)$/u.test(value)) {
    throw badRequest('INVALID_LOCK_VERSION', 'lockVersion must be an unsigned integer string')
  }
  return value
}

export class DefaultGrayRuleService implements GrayRuleService {
  readonly #drafts: GrayRuleDraftRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator

  constructor(
    drafts: GrayRuleDraftRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
  ) {
    this.#drafts = drafts
    this.#environments = environments
    this.#validator = validator
  }

  async getDraft(actor: Actor): Promise<GrayRulesDraftView> {
    const environmentInternalId = await this.#requireEnvironment(actor)
    const draft = await this.#drafts.findByEnvironmentInternalId(environmentInternalId)
    if (!draft) {
      throw notFound('Gray rules draft')
    }
    return draft
  }

  async createDraft(actor: Actor, requestId: string): Promise<GrayRulesDraftView> {
    const environmentInternalId = await this.#requireEditorEnvironment(actor)
    return this.#drafts.getOrCreate(actor, environmentInternalId, requestId)
  }

  async saveDraft(
    actor: Actor,
    input: SaveGrayRulesInput,
    requestId: string,
  ): Promise<GrayRulesDraftView> {
    const environmentInternalId = await this.#requireEditorEnvironment(actor)
    const issues = validateGrayRules(input.document, this.#validator.limits?.grayRules)
    if (issues.length > 0) {
      throw badRequest(
        'INVALID_GRAY_RULES',
        'The gray rules document contains invalid fields',
        issues.map((issue) => ({ path: issue.field, code: issue.code, message: issue.message })),
      )
    }
    return this.#drafts.save(
      actor,
      environmentInternalId,
      parseLockVersion(input.lockVersion),
      input.document as GrayRulesDocument,
      requestId,
    )
  }

  async validatePreview(
    actor: Actor,
    document: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<GrayValidationPreviewView> {
    await this.#requireEnvironment(actor)
    const issues = validateGrayRules(document, this.#validator.limits?.grayRules)
    if (issues.length > 0) {
      return {
        valid: false,
        issues,
        wirePreview: null,
        wireSha256: null,
        dataId: GRAY_MATCH_DATA_ID,
        group: GRAY_MATCH_GROUP,
        ruleCount: 0,
        nativeValid: null,
        nativeErrors: [],
        validator: null,
      }
    }
    const typed = document as GrayRulesDocument
    const wirePreview = compileGrayRulesWire(typed)
    const wireSha256 = sha256(wirePreview).toString('hex')
    const ruleCount = countEffectiveGrayRules(typed)

    if (!this.#validator.available) {
      return {
        valid: true,
        issues: [],
        wirePreview,
        wireSha256,
        dataId: GRAY_MATCH_DATA_ID,
        group: GRAY_MATCH_GROUP,
        ruleCount,
        nativeValid: null,
        nativeErrors: [],
        validator: null,
      }
    }

    const verdict = await this.#validator.validate(
      {
        requestId,
        kind: 'gray_rules',
        project: '',
        payload: Buffer.from(wirePreview, 'utf8'),
      },
      signal,
    )
    return {
      valid: verdict.valid,
      issues: [],
      wirePreview,
      wireSha256,
      dataId: GRAY_MATCH_DATA_ID,
      group: GRAY_MATCH_GROUP,
      ruleCount,
      nativeValid: verdict.valid,
      nativeErrors: verdict.errors,
      validator: {
        contractVersion: verdict.contractVersion,
        revision: verdict.validatorRevision,
      },
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

export class UnavailableGrayRuleService implements GrayRuleService {
  async getDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
  async createDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
  async saveDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
  async validatePreview(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
