// Staged validation for the WAF policy model (field syntax → cross checks).
//
// This layer never decides wire semantics; it rejects malformed authoring
// input early with precise field paths. The authority for expression and CIDR
// semantics is the native validator compile (edge_function_script kind),
// which publication runs fail-closed before any Release.

import { MAX_GLOB_BYTES, MAX_GLOBS_PER_SELECTOR } from './edge-function-model.js'
import { groupModeSets, groupMountSelector } from './group-scope.js'
import type {
  WafGroup,
  WafGroupExemption,
  WafMode,
  WafPolicy,
  WafRule,
  WafSeverity,
} from './model.js'

export interface WafValidationIssue {
  field: string
  code: string
  message: string
}

const SEVERITIES: readonly WafSeverity[] = ['critical', 'high', 'medium', 'low']
const MODES: readonly WafMode[] = ['off', 'log', 'block']
const MAX_RULES = 256
// Every enabled group is one edge function, and a release may mount 64 in all,
// handwritten ones included. Half of that stays free for them.
const MAX_GROUPS = 32
const MAX_NAME_BYTES = 255
const MAX_MATCH_BYTES = 8_192
const MAX_METHODS = 16
const MAX_PATHS = 64
const MAX_CIDRS_PER_RULE = 64
const MAX_PROJECT_MODES = 1_024

const ruleIdPattern = /^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/u
// Leaves room for the `waf-` prefix inside the 64-byte function-name cap.
const groupIdPattern = /^[a-z0-9](?:[a-z0-9-]{0,58}[a-z0-9])?$/u
const methodPattern = /^[A-Z][A-Z0-9]{0,15}$/u
// Exact paths may not contain '*' at all; prefix patterns carry exactly one
// trailing '*'. Printable ASCII, no control characters.
const pathPattern = /^\/[!-~]*$/u

function byteLength(value: string): number {
  return Buffer.byteLength(value, 'utf8')
}

// JSON permits lone surrogate escapes; UTF-8 encoding would silently replace
// them with U+FFFD, so every authoring text field must reject them up front.
// Shared with the edge-function draft validation (edge-function-validate.ts).
export function hasLoneSurrogate(value: string): boolean {
  for (let index = 0; index < value.length; index += 1) {
    const codeUnit = value.charCodeAt(index)
    if (codeUnit >= 0xd800 && codeUnit <= 0xdbff) {
      const next = index + 1 < value.length ? value.charCodeAt(index + 1) : 0
      if (!(next >= 0xdc00 && next <= 0xdfff)) {
        return true
      }
      index += 1
    } else if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff) {
      return true
    }
  }
  return false
}

export function isPrintableSingleLine(value: string): boolean {
  for (let index = 0; index < value.length; index += 1) {
    const codeUnit = value.charCodeAt(index)
    if (codeUnit < 0x20 || codeUnit === 0x7f) {
      return false
    }
  }
  return !hasLoneSurrogate(value)
}

// Mount selector entries are wire globs: printable, no ';' (the projects-list
// wire splits on it), bounded length. '*' wildcard syntax is the codec's to
// judge; anything printable without ';' is accepted here.
export function isProjectGlob(value: string): boolean {
  if (value.length === 0 || byteLength(value) > MAX_GLOB_BYTES) return false
  for (let index = 0; index < value.length; index += 1) {
    const codeUnit = value.charCodeAt(index)
    if (codeUnit < 0x20 || codeUnit === 0x7f || codeUnit === 0x3b) {
      return false
    }
  }
  return !hasLoneSurrogate(value)
}

// projectModes keys are exact names: the native codec requires printable
// non-';' bytes (the projects-list wire splits on ';'), and a '*' here would
// silently stop meaning a single project.
export function isValidProjectName(value: string): boolean {
  if (value.length === 0 || byteLength(value) > 255) {
    return false
  }
  for (let index = 0; index < value.length; index += 1) {
    const codeUnit = value.charCodeAt(index)
    if (codeUnit <= 0x20 || codeUnit === 0x7f || codeUnit === 0x3b || codeUnit === 0x2a) {
      return false
    }
  }
  return !hasLoneSurrogate(value)
}

// Shape-level CIDR check (dotted quad or pragmatic IPv6 group form plus
// prefix length). The native validator's directive compile is the authority
// and rejects anything exotic fail-closed; this check exists to surface
// obvious authoring typos at field-validation time.
export function isValidCidrLiteral(value: string): boolean {
  const separator = value.lastIndexOf('/')
  if (separator <= 0 || separator === value.length - 1) {
    return false
  }
  const address = value.slice(0, separator)
  const prefix = value.slice(separator + 1)
  if (!/^\d{1,3}$/u.test(prefix)) {
    return false
  }
  const prefixLength = Number(prefix)
  if (/^\d{1,3}(\.\d{1,3}){3}$/u.test(address)) {
    if (prefixLength > 32) {
      return false
    }
    return address.split('.').every((part) => {
      const octet = Number(part)
      return octet <= 255 && !(part.length > 1 && part.startsWith('0'))
    })
  }
  // Pragmatic IPv6: hex groups (1-4 digits), at most one '::', optional
  // embedded IPv4 tail. Full structural validation happens natively.
  if (!/^[0-9a-fA-F:]+$/u.test(address)) {
    return false
  }
  const doubleColonCount = (address.match(/::/gu) ?? []).length
  if (doubleColonCount > 1 || address.split(':').length > 8 + 2) {
    return false
  }
  return prefixLength <= 128
}

function validateRule(rule: WafRule, ruleIndex: number, issues: WafValidationIssue[]): void {
  const field = (name: string): string => `rules[${ruleIndex}].${name}`

  if (typeof rule.id !== 'string' || !ruleIdPattern.test(rule.id)) {
    issues.push({
      field: field('id'),
      code: 'invalid_field',
      message: 'rule id must be 1-64 bytes of [a-z0-9-] with alphanumeric edges',
    })
  }
  if (
    typeof rule.name !== 'string' ||
    rule.name.length === 0 ||
    byteLength(rule.name) > MAX_NAME_BYTES ||
    !isPrintableSingleLine(rule.name)
  ) {
    issues.push({
      field: field('name'),
      code: 'invalid_field',
      message: 'rule name must be 1-255 bytes of single-line printable text',
    })
  }
  if (!SEVERITIES.includes(rule.severity)) {
    issues.push({
      field: field('severity'),
      code: 'invalid_field',
      message: `severity must be one of ${SEVERITIES.join(', ')}`,
    })
  }
  if (!Number.isInteger(rule.priority) || rule.priority < 0 || rule.priority > 1_000_000) {
    issues.push({
      field: field('priority'),
      code: 'out_of_range',
      message: 'priority must be an integer within [0, 1000000]',
    })
  }
  if (rule.action !== 'log' && rule.action !== 'block') {
    issues.push({
      field: field('action'),
      code: 'invalid_field',
      message: 'action must be log or block',
    })
  }
  if (rule.phase !== 'request_head') {
    issues.push({
      field: field('phase'),
      code: 'invalid_field',
      message: "phase must be 'request_head' in v1",
    })
  }
  if (
    typeof rule.match !== 'string' ||
    rule.match.trim().length === 0 ||
    byteLength(rule.match) > MAX_MATCH_BYTES ||
    hasLoneSurrogate(rule.match)
  ) {
    issues.push({
      field: field('match'),
      code: 'invalid_field',
      message: `match must be a non-empty dialect expression of at most ${MAX_MATCH_BYTES} bytes`,
    })
  }

  const scope = rule.scope ?? {}
  const methods = scope.methods ?? []
  if (methods.length > MAX_METHODS) {
    issues.push({
      field: field('scope.methods'),
      code: 'limit_exceeded',
      message: `at most ${MAX_METHODS} methods per rule`,
    })
  } else {
    const seen = new Set<string>()
    methods.forEach((method) => {
      if (!methodPattern.test(method)) {
        issues.push({
          field: field('scope.methods'),
          code: 'invalid_field',
          message: `invalid HTTP method '${method}' (uppercase A-Z/0-9, at most 16 chars)`,
        })
      }
      if (seen.has(method)) {
        issues.push({
          field: field('scope.methods'),
          code: 'conflict',
          message: `duplicate method '${method}'`,
        })
      }
      seen.add(method)
    })
  }

  const paths = scope.paths ?? []
  if (paths.length > MAX_PATHS) {
    issues.push({
      field: field('scope.paths'),
      code: 'limit_exceeded',
      message: `at most ${MAX_PATHS} paths per rule`,
    })
  } else {
    const seen = new Set<string>()
    paths.forEach((path) => {
      const bare = path.endsWith('*') ? path.slice(0, -1) : path
      if (!pathPattern.test(bare) || bare.includes('*') || byteLength(path) > 255) {
        issues.push({
          field: field('scope.paths'),
          code: 'invalid_field',
          message: `invalid path pattern '${path}' (exact path or trailing-'*' prefix, printable ASCII)`,
        })
      }
      if (seen.has(path)) {
        issues.push({
          field: field('scope.paths'),
          code: 'conflict',
          message: `duplicate path '${path}'`,
        })
      }
      seen.add(path)
    })
  }

  const cidrs = scope.sourceCidrs ?? []
  if (cidrs.length > MAX_CIDRS_PER_RULE) {
    issues.push({
      field: field('scope.sourceCidrs'),
      code: 'limit_exceeded',
      message: `at most ${MAX_CIDRS_PER_RULE} CIDRs per rule`,
    })
  } else {
    const seen = new Set<string>()
    cidrs.forEach((cidr) => {
      if (!isValidCidrLiteral(cidr)) {
        issues.push({
          field: field('scope.sourceCidrs'),
          code: 'invalid_field',
          message: `invalid CIDR literal '${cidr}'`,
        })
      }
      if (seen.has(cidr)) {
        issues.push({
          field: field('scope.sourceCidrs'),
          code: 'conflict',
          message: `duplicate CIDR '${cidr}'`,
        })
      }
      seen.add(cidr)
    })
  }
}

function validateExemptions(
  exemptions: readonly WafGroupExemption[],
  field: string,
  issues: WafValidationIssue[],
): void {
  const seen = new Set<string>()
  exemptions.forEach((exemption, exemptionIndex) => {
    const entry = `${field}[${exemptionIndex}]`
    if (typeof exemption?.project !== 'string' || !isProjectGlob(exemption.project)) {
      issues.push({
        field: `${entry}.project`,
        code: 'invalid_field',
        message: 'exempt project must be a printable project name or glob without ";"',
      })
    } else if (seen.has(exemption.project)) {
      issues.push({
        field: `${entry}.project`,
        code: 'conflict',
        message: `duplicate exemption for project '${exemption.project}'`,
      })
    }
    seen.add(exemption?.project)
    if (
      typeof exemption?.reason !== 'string' ||
      exemption.reason.trim().length === 0 ||
      byteLength(exemption.reason) > MAX_NAME_BYTES ||
      !isPrintableSingleLine(exemption.reason)
    ) {
      issues.push({
        field: `${entry}.reason`,
        code: 'invalid_field',
        message: 'exemption reason must be 1-255 bytes of single-line printable text',
      })
    }
  })
}

function validateGroup(group: WafGroup, groupIndex: number, issues: WafValidationIssue[]): void {
  const field = (name: string): string => `groups[${groupIndex}].${name}`

  if (typeof group.id !== 'string' || !groupIdPattern.test(group.id)) {
    issues.push({
      field: field('id'),
      code: 'invalid_field',
      message: 'group id must be 1-60 bytes of [a-z0-9-] with alphanumeric edges',
    })
  }
  if (
    typeof group.name !== 'string' ||
    group.name.trim().length === 0 ||
    byteLength(group.name) > MAX_NAME_BYTES ||
    !isPrintableSingleLine(group.name)
  ) {
    issues.push({
      field: field('name'),
      code: 'invalid_field',
      message: 'group name must be 1-255 bytes of single-line printable text',
    })
  }
  if (!MODES.includes(group.mode)) {
    issues.push({
      field: field('mode'),
      code: 'invalid_field',
      message: 'mode must be off, log, or block',
    })
  }
  if (!Number.isInteger(group.priority) || group.priority < 0 || group.priority > 1_000_000) {
    issues.push({
      field: field('priority'),
      code: 'out_of_range',
      message: 'priority must be an integer within [0, 1000000]',
    })
  }

  const include = group.projects?.include
  if (!Array.isArray(include) || include.length === 0) {
    issues.push({
      field: field('projects.include'),
      code: 'invalid_field',
      message: 'a group must apply to at least one project or glob',
    })
  } else if (include.length > MAX_GLOBS_PER_SELECTOR) {
    issues.push({
      field: field('projects.include'),
      code: 'limit_exceeded',
      message: `at most ${MAX_GLOBS_PER_SELECTOR} include entries per group`,
    })
  } else {
    const seen = new Set<string>()
    include.forEach((glob) => {
      if (typeof glob !== 'string' || !isProjectGlob(glob)) {
        issues.push({
          field: field('projects.include'),
          code: 'invalid_field',
          message: `invalid project glob '${String(glob)}'`,
        })
      } else if (seen.has(glob)) {
        issues.push({
          field: field('projects.include'),
          code: 'conflict',
          message: `duplicate project glob '${glob}'`,
        })
      }
      seen.add(glob)
    })
  }

  const exempt = group.projects?.exempt ?? []
  if (!Array.isArray(exempt) || exempt.length > MAX_GLOBS_PER_SELECTOR) {
    issues.push({
      field: field('projects.exempt'),
      code: 'limit_exceeded',
      message: `at most ${MAX_GLOBS_PER_SELECTOR} exemptions per group`,
    })
  } else {
    validateExemptions(exempt, field('projects.exempt'), issues)
  }
}

export function validateWafPolicy(policy: WafPolicy): readonly WafValidationIssue[] {
  const issues: WafValidationIssue[] = []

  if (policy.schemaVersion !== 2) {
    issues.push({
      field: 'schemaVersion',
      code: 'invalid_field',
      message: `unsupported WAF policy schemaVersion ${String(policy.schemaVersion)}, expected 2`,
    })
  }
  if (!Array.isArray(policy.groups) || policy.groups.length > MAX_GROUPS) {
    issues.push({
      field: 'groups',
      code: 'limit_exceeded',
      message: `at most ${MAX_GROUPS} groups`,
    })
    return issues
  }
  if (!Array.isArray(policy.rules) || policy.rules.length > MAX_RULES) {
    issues.push({
      field: 'rules',
      code: 'limit_exceeded',
      message: `at most ${MAX_RULES} rules`,
    })
    return issues
  }
  if (
    policy.projectModes === null ||
    typeof policy.projectModes !== 'object' ||
    Array.isArray(policy.projectModes) ||
    Object.keys(policy.projectModes).length > MAX_PROJECT_MODES
  ) {
    issues.push({
      field: 'projectModes',
      code: 'limit_exceeded',
      message: `projectModes must be an object with at most ${MAX_PROJECT_MODES} entries`,
    })
    return issues
  }

  const groupIds = new Set<string>()
  policy.groups.forEach((group, groupIndex) => {
    if (group === null || typeof group !== 'object') {
      issues.push({
        field: `groups[${groupIndex}]`,
        code: 'invalid_field',
        message: 'group must be an object',
      })
      return
    }
    validateGroup(group, groupIndex, issues)
    if (groupIds.has(group.id)) {
      issues.push({
        field: `groups[${groupIndex}].id`,
        code: 'conflict',
        message: `duplicate group id '${group.id}'`,
      })
    }
    groupIds.add(group.id)
  })

  policy.rules.forEach((rule, ruleIndex) => {
    if (rule === null || typeof rule !== 'object') {
      issues.push({
        field: `rules[${ruleIndex}]`,
        code: 'invalid_field',
        message: 'rule must be an object',
      })
      return
    }
    validateRule(rule, ruleIndex, issues)
    if (!groupIds.has(rule.group)) {
      issues.push({
        field: `rules[${ruleIndex}].group`,
        code: 'unknown_group',
        message: `rule group '${String(rule.group)}' does not exist`,
      })
    }
  })

  const ruleIds = new Set<string>()
  policy.rules.forEach((rule, ruleIndex) => {
    if (typeof rule?.id === 'string' && ruleIds.has(rule.id)) {
      issues.push({
        field: `rules[${ruleIndex}].id`,
        code: 'conflict',
        message: `duplicate rule id '${rule.id}'`,
      })
    }
    if (typeof rule?.id === 'string') {
      ruleIds.add(rule.id)
    }
  })

  for (const [project, mode] of Object.entries(policy.projectModes ?? {})) {
    if (!isValidProjectName(project)) {
      issues.push({
        field: `projectModes['${project}']`,
        code: 'invalid_field',
        message: "project mode key must be a printable project name without '*' or ';'",
      })
    }
    if (!MODES.includes(mode)) {
      issues.push({
        field: `projectModes['${project}']`,
        code: 'invalid_field',
        message: `invalid mode '${String(mode)}' for project '${project}'`,
      })
    }
  }

  // The mount exclude list merges exemptions with off-mode projects, so only
  // the whole policy can tell whether it fits the wire selector cap.
  if (issues.length === 0) {
    policy.groups.forEach((group, groupIndex) => {
      const { exclude } = groupMountSelector(group, groupModeSets(policy, group))
      if (exclude.length > MAX_GLOBS_PER_SELECTOR) {
        issues.push({
          field: `groups[${groupIndex}].projects.exempt`,
          code: 'limit_exceeded',
          message: `exempt and off-mode projects total ${exclude.length}, above the ${MAX_GLOBS_PER_SELECTOR}-entry mount limit`,
        })
      }
    })
  }

  return issues
}
