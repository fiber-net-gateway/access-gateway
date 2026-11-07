// Handwritten edge-function service: staged validation → draft persistence →
// compile preview. Field-level checks (edge-function-validate.ts) gate saves;
// the native validator compile (kind 'edge_function_script') is the
// fail-closed authority for dialect semantics and script limits, exactly like
// the WAF policy compile preview. Releases merge these functions with the
// generated WAF function (release-service.ts).

import type { NativeValidator } from '../../integrations/native-validator/model.js'
import { sha256 } from '../../shared/json.js'
import { badRequest, forbidden, notFound, unavailable } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import type { EdgeFunctionsDraft } from './edge-function-model.js'
import {
  EdgeFunctionDraftRepository,
  type EdgeFunctionDraftView,
} from './edge-function-repository.js'
import {
  validateEdgeFunctionsDraft,
  type EdgeFunctionValidationIssue,
} from './edge-function-validate.js'

export type { EdgeFunctionDraftView } from './edge-function-repository.js'

export interface SaveEdgeFunctionDraftInput {
  lockVersion: string
  draft: unknown
}

export interface EdgeFunctionCompileResult {
  readonly name: string
  /** Execution phase the script was compiled in (always concrete here). */
  readonly phase: 'request_head' | 'request_body'
  /** sha256 over the exact script bytes the release would publish. */
  readonly scriptSha256: string
  readonly scriptValid: boolean
  readonly errors: readonly {
    code: string
    field?: string
    offset?: number
    message: string
  }[]
}

export interface EdgeFunctionCompilePreviewView {
  /** False when the draft has field-level issues; functions is then empty. */
  readonly valid: boolean
  readonly issues: readonly EdgeFunctionValidationIssue[]
  /** One entry per ENABLED function, in declaration order. */
  readonly functions: readonly EdgeFunctionCompileResult[]
  readonly validator: { contractVersion: number; revision: string } | null
}

export interface EdgeFunctionService {
  getDraft(actor: Actor): Promise<EdgeFunctionDraftView>
  createDraft(actor: Actor, requestId: string): Promise<EdgeFunctionDraftView>
  saveDraft(
    actor: Actor,
    input: SaveEdgeFunctionDraftInput,
    requestId: string,
  ): Promise<EdgeFunctionDraftView>
  compilePreview(
    actor: Actor,
    draft: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<EdgeFunctionCompilePreviewView>
}

function parseDraft(value: unknown): EdgeFunctionsDraft {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw badRequest('INVALID_EDGE_FUNCTION_DRAFT', 'Edge function draft must be an object', [
      { path: 'draft', code: 'invalid_field', message: 'Edge function draft must be an object' },
    ])
  }
  const issues = validateEdgeFunctionsDraft(value)
  if (issues.length > 0) {
    throw badRequest(
      'INVALID_EDGE_FUNCTION_DRAFT',
      'The edge-function draft contains invalid fields',
      issues.map((issue) => ({ path: issue.field, code: issue.code, message: issue.message })),
    )
  }
  return value as EdgeFunctionsDraft
}

function parseLockVersion(value: string): string {
  if (!/^(0|[1-9][0-9]*)$/u.test(value)) {
    throw badRequest('INVALID_LOCK_VERSION', 'lockVersion must be an unsigned integer string')
  }
  return value
}

export class DefaultEdgeFunctionService implements EdgeFunctionService {
  readonly #drafts: EdgeFunctionDraftRepository
  readonly #environments: EnvironmentRepository
  readonly #validator: NativeValidator

  constructor(
    drafts: EdgeFunctionDraftRepository,
    environments: EnvironmentRepository,
    validator: NativeValidator,
  ) {
    this.#drafts = drafts
    this.#environments = environments
    this.#validator = validator
  }

  async getDraft(actor: Actor): Promise<EdgeFunctionDraftView> {
    const environmentInternalId = await this.#requireEnvironment(actor)
    const draft = await this.#drafts.findByEnvironmentInternalId(environmentInternalId)
    if (!draft) {
      throw notFound('Edge function draft')
    }
    return draft
  }

  async createDraft(actor: Actor, requestId: string): Promise<EdgeFunctionDraftView> {
    const environmentInternalId = await this.#requireEditorEnvironment(actor)
    return this.#drafts.getOrCreate(actor, environmentInternalId, requestId)
  }

  async saveDraft(
    actor: Actor,
    input: SaveEdgeFunctionDraftInput,
    requestId: string,
  ): Promise<EdgeFunctionDraftView> {
    const environmentInternalId = await this.#requireEditorEnvironment(actor)
    const draft = parseDraft(input.draft)
    return this.#drafts.save(
      actor,
      environmentInternalId,
      parseLockVersion(input.lockVersion),
      draft,
      requestId,
    )
  }

  async compilePreview(
    actor: Actor,
    draft: unknown,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<EdgeFunctionCompilePreviewView> {
    await this.#requireEditorEnvironment(actor)
    const issues = validateEdgeFunctionsDraft(draft)
    if (issues.length > 0) {
      return { valid: false, issues, functions: [], validator: null }
    }
    const functions = (draft as EdgeFunctionsDraft).functions.filter((fn) => fn.enabled)
    const results: EdgeFunctionCompileResult[] = []
    let lastValidator: { contractVersion: number; revision: string } | null = null
    for (const fn of functions) {
      // The phase decides whether $req.body resolves: a head-phase script
      // referencing it must fail the preview, not pass and break at runtime.
      const phase = fn.phase ?? 'request_head'
      const native = await this.#validator.validate(
        {
          requestId,
          kind: 'edge_function_script',
          project: '',
          payload: Buffer.from(fn.script, 'utf8'),
          ...(phase !== 'request_head' ? { phase } : {}),
        },
        signal,
      )
      lastValidator = {
        contractVersion: native.contractVersion,
        revision: native.validatorRevision,
      }
      results.push({
        name: fn.name,
        phase,
        scriptSha256: sha256(fn.script).toString('hex'),
        scriptValid: native.valid,
        errors: native.errors.map((error) => ({
          code: error.code,
          ...(error.field !== undefined ? { field: error.field } : {}),
          ...(error.offset !== undefined ? { offset: error.offset } : {}),
          message: error.message,
        })),
      })
    }
    return { valid: true, issues: [], functions: results, validator: lastValidator }
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

export class UnavailableEdgeFunctionService implements EdgeFunctionService {
  async getDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async createDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async saveDraft(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }

  async compilePreview(): Promise<never> {
    throw unavailable('DATABASE_UNCONFIGURED', 'MySQL is not configured')
  }
}
