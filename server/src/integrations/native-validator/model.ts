export type NativeValidationKind = 'project_route' | 'gray_rules'

export interface NativeValidationError {
  code: string
  field?: string
  offset?: number
  message: string
}

export interface NativeValidationRequest {
  requestId: string
  kind: NativeValidationKind
  project: string
  payload: Uint8Array
}

export interface NativeValidationResult {
  valid: boolean
  normalized?: Readonly<Record<string, unknown>>
  errors: readonly NativeValidationError[]
  contractVersion: number
  validatorRevision: string
}

export interface NativeValidator {
  readonly available: boolean
  readonly contractVersion: number
  readonly revision: string | null
  validate(request: NativeValidationRequest, signal?: AbortSignal): Promise<NativeValidationResult>
}
