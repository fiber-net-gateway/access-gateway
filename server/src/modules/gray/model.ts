// Environment-wide gray routing authoring model and deterministic wire compiler
// for `ploto.unified-access.gray-match` (group `DEFAULT_GROUP`).
//
// The native codec (native/access-server/src/config/GrayMatchConfigCodec.cpp +
// GrayMatchCompiler.cpp) is the source of truth for wire format:
//   - Top-level JSON object keyed by recognized entry network
//     ('internet' | 'vdi' | 'desktop' | 'custom')
//   - Each entry value is { ratio: 0..10000, cidrs: string[] }
//   - Empty JSON object `{}` explicitly clears all gray rules (never publish an
//     empty string, because the native codec treats `""` as keep-current).

import { canonicalJson } from '../../shared/json.js'

export const GRAY_MATCH_DATA_ID = 'ploto.unified-access.gray-match'
export const GRAY_MATCH_GROUP = 'DEFAULT_GROUP'
export const GRAY_COMPILER_REVISION = 'gray-match-v1'
export const GRAY_RATIO_MAX = 10_000

export const GRAY_RULE_ENTRIES = ['internet', 'vdi', 'desktop', 'custom'] as const
export type GrayRuleEntry = (typeof GRAY_RULE_ENTRIES)[number]

export interface GrayRule {
  entry: GrayRuleEntry
  /** Ratio out of 10,000 (e.g. 500 = 5.00%, 10000 = 100.00%). */
  ratio: number
  /** IPv4/IPv6 CIDR literals (e.g. '10.0.0.0/8', '2001:db8::/32'). */
  cidrs: readonly string[]
}

export interface GrayRulesDocument {
  schemaVersion: 1
  rules: readonly GrayRule[]
}

export interface GrayRuleWireEntry {
  ratio: number
  cidrs: readonly string[]
}

export function emptyGrayRulesDocument(): GrayRulesDocument {
  return { schemaVersion: 1, rules: [] }
}

export function isGrayRuleEntry(value: unknown): value is GrayRuleEntry {
  return typeof value === 'string' && (GRAY_RULE_ENTRIES as readonly string[]).includes(value)
}

/**
 * Compile the Console authoring model into the deterministic wire JSON consumed
 * by `access-server` (`GrayMatchConfigCodec.cpp`).
 */
export function compileGrayRulesWire(doc: GrayRulesDocument): string {
  const byEntry = new Map<GrayRuleEntry, GrayRule>()
  for (const rule of doc.rules) {
    byEntry.set(rule.entry, rule)
  }
  const wire: Record<string, GrayRuleWireEntry> = {}
  for (const entry of GRAY_RULE_ENTRIES) {
    const rule = byEntry.get(entry)
    if (!rule) continue
    wire[entry] = {
      ratio: rule.ratio,
      cidrs: [...rule.cidrs],
    }
  }
  return canonicalJson(wire)
}

/**
 * Count rules that are actually effective in `GrayMatchCompiler.cpp`
 * (rules with ratio === 0 && cidrs.length === 0 are ignored by the compiler).
 */
export function countEffectiveGrayRules(doc: GrayRulesDocument): number {
  return doc.rules.filter((rule) => rule.ratio > 0 || rule.cidrs.length > 0).length
}

export interface ParsedGrayWireResult {
  document: GrayRulesDocument | null
  parseError: string | null
}

/**
 * Defensively parse live rnacos `ploto.unified-access.gray-match` JSON into the
 * Console authoring model for live comparison and rollback reconstruction.
 */
export function parseGrayRulesWire(content: string): ParsedGrayWireResult {
  if (content === '') {
    return {
      document: null,
      parseError: 'content is empty (native runtime keeps current snapshot)',
    }
  }
  let decoded: unknown
  try {
    decoded = JSON.parse(content) as unknown
  } catch (error) {
    return {
      document: null,
      parseError: `content is not valid JSON: ${error instanceof Error ? error.message : 'unknown error'}`,
    }
  }
  if (decoded === null) {
    return { document: emptyGrayRulesDocument(), parseError: null }
  }
  if (typeof decoded !== 'object' || Array.isArray(decoded)) {
    return { document: null, parseError: 'gray-match payload must be a JSON object or null' }
  }
  const record = decoded as Record<string, unknown>
  const rules: GrayRule[] = []
  const byEntry = new Map<GrayRuleEntry, GrayRule>()
  for (const [entryKey, rawValue] of Object.entries(record)) {
    if (!isGrayRuleEntry(entryKey)) {
      return {
        document: null,
        parseError: `unrecognized gray entry "${entryKey}"`,
      }
    }
    if (typeof rawValue !== 'object' || rawValue === null || Array.isArray(rawValue)) {
      return {
        document: null,
        parseError: `gray entry "${entryKey}" must be a JSON object`,
      }
    }
    const entryObj = rawValue as Record<string, unknown>
    const ratio = entryObj.ratio === undefined ? 0 : entryObj.ratio
    if (
      typeof ratio !== 'number' ||
      !Number.isInteger(ratio) ||
      ratio < 0 ||
      ratio > GRAY_RATIO_MAX
    ) {
      return {
        document: null,
        parseError: `gray entry "${entryKey}" has invalid ratio`,
      }
    }
    const cidrsRaw = entryObj.cidrs === undefined ? [] : entryObj.cidrs
    if (!Array.isArray(cidrsRaw) || !cidrsRaw.every((item) => typeof item === 'string')) {
      return {
        document: null,
        parseError: `gray entry "${entryKey}" has invalid cidrs list`,
      }
    }
    byEntry.set(entryKey, {
      entry: entryKey,
      ratio,
      cidrs: [...cidrsRaw],
    })
  }
  for (const entry of GRAY_RULE_ENTRIES) {
    const rule = byEntry.get(entry)
    if (rule) rules.push(rule)
  }
  return {
    document: { schemaVersion: 1, rules },
    parseError: null,
  }
}
