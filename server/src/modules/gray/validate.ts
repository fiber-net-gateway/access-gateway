// Staged validation for the Gray Match authoring model.
//
// Stage 1/2 (this module) catches field syntax, duplicates, ratio bounds, CIDR
// syntax, and configured limits with precise field paths before saving drafts.
// Stage 3 (Native Validator `kind: 'gray_rules'`) is the fail-closed authority
// for exact native C++ codec + CIDR compilation semantics before Release.

import { isIPv4, isIPv6 } from 'node:net'

import type { AccessConfigLimits } from '../../integrations/native-validator/model.js'
import {
  compileGrayRulesWire,
  GRAY_RATIO_MAX,
  GRAY_RULE_ENTRIES,
  isGrayRuleEntry,
  type GrayRulesDocument,
} from './model.js'

export interface GrayValidationIssue {
  field: string
  code: string
  message: string
}

const DEFAULT_MAX_PAYLOAD_BYTES = 1024 * 1024
const DEFAULT_MAX_RULES = 64
const DEFAULT_MAX_CIDRS_PER_RULE = 4_096
const DEFAULT_MAX_CIDR_BYTES = 128

/**
 * Strict IPv4/IPv6 CIDR check aligned with native `Cidr::parse`:
 * `<ip>/<prefix>` with no whitespace, canonical decimal prefix length without
 * leading signs, valid IPv4 (0..32) or IPv6 (0..128) address.
 */
export function isValidGrayCidr(value: string): boolean {
  if (value.length === 0 || /\s/u.test(value)) {
    return false
  }
  const slashIndex = value.indexOf('/')
  if (slashIndex <= 0 || slashIndex !== value.lastIndexOf('/') || slashIndex === value.length - 1) {
    return false
  }
  const address = value.slice(0, slashIndex)
  const prefixText = value.slice(slashIndex + 1)
  if (!/^(0|[1-9][0-9]{0,2})$/u.test(prefixText)) {
    return false
  }
  const prefix = Number(prefixText)
  if (isIPv4(address)) {
    return prefix >= 0 && prefix <= 32
  }
  if (isIPv6(address)) {
    return prefix >= 0 && prefix <= 128
  }
  return false
}

export function validateGrayRules(
  value: unknown,
  limits?: AccessConfigLimits['grayRules'] | null,
): readonly GrayValidationIssue[] {
  const issues: GrayValidationIssue[] = []
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    return [
      {
        field: 'document',
        code: 'invalid_field',
        message: 'gray rules document must be a JSON object',
      },
    ]
  }
  const record = value as Record<string, unknown>
  if (record.schemaVersion !== 1) {
    issues.push({
      field: 'schemaVersion',
      code: 'invalid_field',
      message: `unsupported gray rules schemaVersion ${String(record.schemaVersion)}, expected 1`,
    })
  }
  if (!Array.isArray(record.rules)) {
    issues.push({
      field: 'rules',
      code: 'invalid_field',
      message: 'rules must be an array',
    })
    return issues
  }

  const maxRules = limits?.maxRules ?? DEFAULT_MAX_RULES
  const maxCidrsPerRule = limits?.maxCidrsPerRule ?? DEFAULT_MAX_CIDRS_PER_RULE
  const maxCidrBytes = limits?.maxCidrBytes ?? DEFAULT_MAX_CIDR_BYTES
  const maxPayloadBytes = limits?.maxPayloadBytes ?? DEFAULT_MAX_PAYLOAD_BYTES

  if (record.rules.length > maxRules) {
    issues.push({
      field: 'rules',
      code: 'limit_exceeded',
      message: `gray rule count exceeds the limit of ${maxRules}`,
    })
  }

  const seenEntries = new Set<string>()
  record.rules.forEach((rawRule, ruleIndex) => {
    const prefix = `rules[${ruleIndex}]`
    if (typeof rawRule !== 'object' || rawRule === null || Array.isArray(rawRule)) {
      issues.push({
        field: prefix,
        code: 'invalid_field',
        message: 'gray rule must be an object',
      })
      return
    }
    const rule = rawRule as Record<string, unknown>
    if (!isGrayRuleEntry(rule.entry)) {
      issues.push({
        field: `${prefix}.entry`,
        code: 'invalid_field',
        message: `entry must be one of ${GRAY_RULE_ENTRIES.join(', ')}`,
      })
    } else if (seenEntries.has(rule.entry)) {
      issues.push({
        field: `${prefix}.entry`,
        code: 'conflict',
        message: `duplicate gray rule entry "${rule.entry}"`,
      })
    } else {
      seenEntries.add(rule.entry)
    }

    if (
      typeof rule.ratio !== 'number' ||
      !Number.isInteger(rule.ratio) ||
      rule.ratio < 0 ||
      rule.ratio > GRAY_RATIO_MAX
    ) {
      issues.push({
        field: `${prefix}.ratio`,
        code: 'out_of_range',
        message: `ratio must be an integer between 0 and ${GRAY_RATIO_MAX}`,
      })
    }

    if (!Array.isArray(rule.cidrs)) {
      issues.push({
        field: `${prefix}.cidrs`,
        code: 'invalid_field',
        message: 'cidrs must be an array of CIDR strings',
      })
      return
    }

    if (rule.cidrs.length > maxCidrsPerRule) {
      issues.push({
        field: `${prefix}.cidrs`,
        code: 'limit_exceeded',
        message: `cidrs count exceeds the limit of ${maxCidrsPerRule} per rule`,
      })
    }

    const seenCidrs = new Set<string>()
    rule.cidrs.forEach((rawCidr, cidrIndex) => {
      const cidrField = `${prefix}.cidrs[${cidrIndex}]`
      if (typeof rawCidr !== 'string') {
        issues.push({
          field: cidrField,
          code: 'invalid_field',
          message: 'CIDR must be a string',
        })
        return
      }
      if (Buffer.byteLength(rawCidr, 'utf8') > maxCidrBytes) {
        issues.push({
          field: cidrField,
          code: 'limit_exceeded',
          message: `CIDR exceeds the ${maxCidrBytes}-byte limit`,
        })
        return
      }
      if (!isValidGrayCidr(rawCidr)) {
        issues.push({
          field: cidrField,
          code: 'invalid_field',
          message: `invalid IPv4/IPv6 CIDR "${rawCidr}"`,
        })
        return
      }
      if (seenCidrs.has(rawCidr)) {
        issues.push({
          field: cidrField,
          code: 'conflict',
          message: `duplicate CIDR "${rawCidr}"`,
        })
        return
      }
      seenCidrs.add(rawCidr)
    })
  })

  if (issues.length === 0) {
    const wire = compileGrayRulesWire(value as GrayRulesDocument)
    const wireBytes = Buffer.byteLength(wire, 'utf8')
    if (wireBytes > maxPayloadBytes) {
      issues.push({
        field: 'payload',
        code: 'limit_exceeded',
        message: `compiled gray rules payload (${wireBytes} bytes) exceeds the ${maxPayloadBytes}-byte limit`,
      })
    }
  }

  return issues
}
