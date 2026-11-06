export type NativeValidationKind =
  'project_route' | 'gray_rules' | 'edge_function_index' | 'edge_function_script'

export interface AccessConfigLimits {
  schemaVersion: 3
  projectList: {
    maxPayloadBytes: number
    maxProjects: number
    maxProjectNameBytes: number
  }
  projectRoute: {
    maxPayloadBytes: number
    maxHosts: number
    maxRoutes: number
    maxHostPatternBytes: number
    maxPathBytes: number
    maxMethodBytes: number
    maxServiceBytes: number
    maxClusterBytes: number
    maxConditionBytes: number
    maxScriptBytes: number
    maxTemplateBytes: number
    maxHeaderEntries: number
    maxHeaderNameBytes: number
    maxHeaderValueBytes: number
    maxCidrsPerRoute: number
    maxCidrBytes: number
    maxAddressesPerRoute: number
    maxAddressBytes: number
    maxUpstreamTlsProfiles: number
    maxUpstreamTlsCaPemBytes: number
    maxStaticResponseBodyBytes: number
    maxStaticResponseBytes: number
    maxPathVariables: number
    maxTemplateExpressions: number
    maxCompiledPrograms: number
    maxEstimatedSnapshotBytes: number
  }
  grayRules: {
    maxPayloadBytes: number
    maxRules: number
    maxEntryBytes: number
    maxCidrsPerRule: number
    maxCidrBytes: number
  }
  edgeFunctions: {
    maxPayloadBytes: number
    maxFunctions: number
    maxFunctionIdBytes: number
    maxFunctionNameBytes: number
    maxMountsPerFunction: number
    maxGlobsPerSelector: number
    maxGlobBytes: number
    maxScriptPayloadBytes: number
    maxTotalScriptBytes: number
    maxReportsPerExecution: number
    maxCidrDirectivesPerScript: number
    maxCidrsPerDirective: number
    maxCidrBytes: number
    maxInputFieldBytes: number
  }
}

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
  readonly limits: AccessConfigLimits | null
  validate(request: NativeValidationRequest, signal?: AbortSignal): Promise<NativeValidationResult>
}
