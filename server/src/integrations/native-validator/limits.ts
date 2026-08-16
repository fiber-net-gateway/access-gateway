import type { AccessConfigLimits } from './model.js'

const projectListKeys = ['maxPayloadBytes', 'maxProjects', 'maxProjectNameBytes'] as const
const projectRouteKeys = [
  'maxPayloadBytes',
  'maxHosts',
  'maxRoutes',
  'maxHostPatternBytes',
  'maxPathBytes',
  'maxMethodBytes',
  'maxServiceBytes',
  'maxClusterBytes',
  'maxConditionBytes',
  'maxScriptBytes',
  'maxTemplateBytes',
  'maxHeaderEntries',
  'maxHeaderNameBytes',
  'maxHeaderValueBytes',
  'maxCidrsPerRoute',
  'maxCidrBytes',
  'maxAddressesPerRoute',
  'maxAddressBytes',
  'maxStaticResponseBodyBytes',
  'maxStaticResponseBytes',
  'maxPathVariables',
  'maxTemplateExpressions',
  'maxCompiledPrograms',
  'maxEstimatedSnapshotBytes',
] as const
const grayRuleKeys = [
  'maxPayloadBytes',
  'maxRules',
  'maxEntryBytes',
  'maxCidrsPerRule',
  'maxCidrBytes',
] as const

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function hasPositiveSafeIntegers(value: Record<string, unknown>, keys: readonly string[]): boolean {
  return keys.every(
    (key) =>
      typeof value[key] === 'number' &&
      Number.isSafeInteger(value[key]) &&
      (value[key] as number) > 0,
  )
}

function hasExactKeys(value: Record<string, unknown>, keys: readonly string[]): boolean {
  const actual = Object.keys(value).sort()
  const expected = [...keys].sort()
  return actual.length === expected.length && actual.every((key, index) => key === expected[index])
}

function isAtMost(value: unknown, upperBound: unknown): boolean {
  return typeof value === 'number' && typeof upperBound === 'number' && value <= upperBound
}

export function isAccessConfigLimits(value: unknown): value is AccessConfigLimits {
  if (!isRecord(value) || value.schemaVersion !== 1) return false
  if (!isRecord(value.projectList) || !isRecord(value.projectRoute) || !isRecord(value.grayRules)) {
    return false
  }
  if (!hasExactKeys(value, ['schemaVersion', 'projectList', 'projectRoute', 'grayRules'])) {
    return false
  }
  return (
    hasExactKeys(value.projectList, projectListKeys) &&
    hasPositiveSafeIntegers(value.projectList, projectListKeys) &&
    hasExactKeys(value.projectRoute, projectRouteKeys) &&
    hasPositiveSafeIntegers(value.projectRoute, projectRouteKeys) &&
    hasExactKeys(value.grayRules, grayRuleKeys) &&
    hasPositiveSafeIntegers(value.grayRules, grayRuleKeys) &&
    isAtMost(value.projectList.maxProjectNameBytes, value.projectList.maxPayloadBytes) &&
    isAtMost(
      value.projectRoute.maxStaticResponseBodyBytes,
      value.projectRoute.maxStaticResponseBytes,
    ) &&
    isAtMost(value.grayRules.maxCidrBytes, value.grayRules.maxPayloadBytes)
  )
}

// Used only when the Native Validator is unavailable, so drafts retain a
// bounded transport envelope. Publication still fails closed without a
// validator revision and its probed limits.
export const fallbackAccessConfigLimits: AccessConfigLimits = {
  schemaVersion: 1,
  projectList: {
    maxPayloadBytes: 262_144,
    maxProjects: 1_024,
    maxProjectNameBytes: 255,
  },
  projectRoute: {
    maxPayloadBytes: 4_194_304,
    maxHosts: 1_024,
    maxRoutes: 5_000,
    maxHostPatternBytes: 255,
    maxPathBytes: 2_048,
    maxMethodBytes: 64,
    maxServiceBytes: 1_024,
    maxClusterBytes: 255,
    maxConditionBytes: 262_144,
    maxScriptBytes: 1_048_576,
    maxTemplateBytes: 1_048_576,
    maxHeaderEntries: 256,
    maxHeaderNameBytes: 256,
    maxHeaderValueBytes: 65_536,
    maxCidrsPerRoute: 256,
    maxCidrBytes: 64,
    maxAddressesPerRoute: 256,
    maxAddressBytes: 2_048,
    maxStaticResponseBodyBytes: 2_097_152,
    maxStaticResponseBytes: 8_388_608,
    maxPathVariables: 64,
    maxTemplateExpressions: 256,
    maxCompiledPrograms: 20_000,
    maxEstimatedSnapshotBytes: 67_108_864,
  },
  grayRules: {
    maxPayloadBytes: 262_144,
    maxRules: 16,
    maxEntryBytes: 64,
    maxCidrsPerRule: 256,
    maxCidrBytes: 64,
  },
}

export function utf8Bytes(value: string): number {
  return Buffer.byteLength(value, 'utf8')
}
