import type { NativeValidator } from '../../integrations/native-validator/model.js'
import type { ProjectRoutesModel } from './model.js'
import { compileProjectRoutes, type RouteValidationIssue } from './compiler.js'

export interface ProjectRoutesValidationView {
  valid: boolean
  issues: readonly RouteValidationIssue[]
  wirePreview: string | null
  wireSha256: string | null
  validator: {
    contractVersion: number
    revision: string
  } | null
}

export async function validateProjectRoutesCandidate(
  validator: NativeValidator,
  domain: string,
  fallbackRouteId: string,
  model: ProjectRoutesModel,
  requestId: string,
  version = 1,
  signal?: AbortSignal,
): Promise<ProjectRoutesValidationView> {
  const result = compileProjectRoutes(domain, model, version)
  if (!result.compiled) {
    return {
      valid: false,
      issues: result.issues,
      wirePreview: null,
      wireSha256: null,
      validator: null,
    }
  }
  const native = await validator.validate(
    {
      requestId,
      kind: 'project_route',
      project: domain,
      payload: result.compiled.payload,
    },
    signal,
  )
  const issues: RouteValidationIssue[] = native.errors.map((error) => {
    const match = /^routes\[(\d+)\](?:\.(.*))?$/u.exec(error.field ?? '')
    const route = match?.[1] ? model.routes[Number(match[1])] : undefined
    return {
      routeId: route?.id ?? model.routes[0]?.id ?? fallbackRouteId,
      path: match?.[2] ?? error.field ?? '',
      line: 1,
      column: 1,
      code: error.code,
      message: error.message,
    }
  })
  return {
    valid: native.valid,
    issues,
    wirePreview: result.compiled.payloadText,
    wireSha256: result.compiled.sha256,
    validator: {
      contractVersion: native.contractVersion,
      revision: native.validatorRevision,
    },
  }
}
