// Staged validation for the handwritten edge-function draft
// (edge-function-model.ts): field syntax and cross-function checks with
// precise field paths. This layer never decides dialect or wire semantics —
// the native validator compile of each script (edge_function_script kind) and
// the index codec (edge_function_index kind) are the fail-closed authority at
// compile-preview and release time.

import {
  EDGE_FUNCTION_PHASES,
  EDGE_FUNCTION_PRIORITY_MAX,
  EDGE_FUNCTION_PRIORITY_MIN,
  EDGE_FUNCTION_RATIO_MAX,
  EDGE_FUNCTION_RATIO_MIN,
  MAX_DESCRIPTION_BYTES,
  MAX_DRAFT_FUNCTIONS,
  MAX_GLOB_BYTES,
  MAX_GLOBS_PER_SELECTOR,
  MAX_MOUNTS_PER_FUNCTION,
  MAX_SCRIPT_BYTES_PER_FUNCTION,
  MAX_TOTAL_DRAFT_SCRIPT_BYTES,
  RESERVED_FUNCTION_NAMES,
  type EdgeFunctionDraftFunction,
  type EdgeFunctionsDraft,
} from './edge-function-model.js'
import { hasLoneSurrogate, isPrintableSingleLine, type WafValidationIssue } from './validate.js'

export type EdgeFunctionValidationIssue = WafValidationIssue

const functionNamePattern = /^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/u

function byteLength(value: string): number {
  return Buffer.byteLength(value, 'utf8')
}

// Mount selector entries are wire globs: printable, no ';'(the projects-list
// wire splits on it), bounded length. '*' wildcard syntax is the codec's to
// judge; anything printable without ';' is accepted here.
function isGlobCandidate(value: string): boolean {
  if (value.length === 0 || byteLength(value) > MAX_GLOB_BYTES) return false
  for (let index = 0; index < value.length; index += 1) {
    const codeUnit = value.charCodeAt(index)
    if (codeUnit < 0x20 || codeUnit === 0x7f || codeUnit === 0x3b) {
      return false
    }
  }
  return !hasLoneSurrogate(value)
}

export function validateEdgeFunctionsDraft(draft: unknown): EdgeFunctionValidationIssue[] {
  const issues: EdgeFunctionValidationIssue[] = []
  if (typeof draft !== 'object' || draft === null || Array.isArray(draft)) {
    return [{ field: 'draft', code: 'invalid_field', message: 'draft must be an object' }]
  }
  const record = draft as Partial<EdgeFunctionsDraft>
  if (record.schemaVersion !== 1) {
    return [
      {
        field: 'schemaVersion',
        code: 'invalid_field',
        message: 'schemaVersion must be 1',
      },
    ]
  }
  if (!Array.isArray(record.functions)) {
    return [{ field: 'functions', code: 'invalid_field', message: 'functions must be an array' }]
  }
  if (record.functions.length > MAX_DRAFT_FUNCTIONS) {
    issues.push({
      field: 'functions',
      code: 'too_many_entries',
      message: `the draft may hold at most ${MAX_DRAFT_FUNCTIONS} functions`,
    })
  }

  const seenNames = new Set<string>()
  let totalScriptBytes = 0
  record.functions.forEach((fn, index) => {
    const path = `functions[${index}]`
    if (typeof fn !== 'object' || fn === null || Array.isArray(fn)) {
      issues.push({ field: path, code: 'invalid_field', message: 'function must be an object' })
      return
    }
    const function_ = fn as Partial<EdgeFunctionDraftFunction>

    if (typeof function_.name !== 'string' || !functionNamePattern.test(function_.name)) {
      issues.push({
        field: `${path}.name`,
        code: 'invalid_function_name',
        message: 'name must be a slug: [a-z0-9-], 1-64 bytes, alphanumeric edges',
      })
    } else {
      if (seenNames.has(function_.name)) {
        issues.push({
          field: `${path}.name`,
          code: 'duplicate_function_name',
          message: `function name "${function_.name}" is used more than once`,
        })
      }
      seenNames.add(function_.name)
      if (RESERVED_FUNCTION_NAMES.includes(function_.name)) {
        issues.push({
          field: `${path}.name`,
          code: 'reserved_function_name',
          message: `function name "${function_.name}" is reserved for the generated WAF function`,
        })
      }
    }

    if (
      function_.description !== undefined &&
      (typeof function_.description !== 'string' ||
        !isPrintableSingleLine(function_.description) ||
        byteLength(function_.description) > MAX_DESCRIPTION_BYTES)
    ) {
      issues.push({
        field: `${path}.description`,
        code: 'invalid_field',
        message: `description must be printable text of at most ${MAX_DESCRIPTION_BYTES} bytes`,
      })
    }

    if (typeof function_.enabled !== 'boolean') {
      issues.push({
        field: `${path}.enabled`,
        code: 'invalid_field',
        message: 'enabled must be a boolean',
      })
    }

    if (
      function_.phase !== undefined &&
      !EDGE_FUNCTION_PHASES.includes(function_.phase as (typeof EDGE_FUNCTION_PHASES)[number])
    ) {
      issues.push({
        field: `${path}.phase`,
        code: 'invalid_field',
        message: `phase must be one of ${EDGE_FUNCTION_PHASES.join(', ')}`,
      })
    }

    if (typeof function_.script !== 'string' || function_.script.length === 0) {
      issues.push({
        field: `${path}.script`,
        code: 'invalid_field',
        message: 'script must be a non-empty string',
      })
    } else if (hasLoneSurrogate(function_.script)) {
      issues.push({
        field: `${path}.script`,
        code: 'invalid_field',
        message: 'script must not contain lone surrogate code units',
      })
    } else {
      const scriptBytes = byteLength(function_.script)
      if (scriptBytes > MAX_SCRIPT_BYTES_PER_FUNCTION) {
        issues.push({
          field: `${path}.script`,
          code: 'limit_exceeded',
          message: `script exceeds ${MAX_SCRIPT_BYTES_PER_FUNCTION} bytes`,
        })
      }
      totalScriptBytes += scriptBytes
    }

    if (!Array.isArray(function_.mounts)) {
      issues.push({
        field: `${path}.mounts`,
        code: 'invalid_field',
        message: 'mounts must be an array',
      })
      return
    }
    if (function_.mounts.length > MAX_MOUNTS_PER_FUNCTION) {
      issues.push({
        field: `${path}.mounts`,
        code: 'too_many_entries',
        message: `a function may hold at most ${MAX_MOUNTS_PER_FUNCTION} mounts`,
      })
    }
    if (function_.enabled === true && function_.mounts.length === 0) {
      issues.push({
        field: `${path}.mounts`,
        code: 'missing_field',
        message: 'an enabled function needs at least one mount',
      })
    }
    function_.mounts.forEach((mount, mountIndex) => {
      validateMount(mount, `${path}.mounts[${mountIndex}]`, issues)
    })
  })

  if (totalScriptBytes > MAX_TOTAL_DRAFT_SCRIPT_BYTES) {
    issues.push({
      field: 'functions',
      code: 'limit_exceeded',
      message: `total script bytes exceed the draft transport budget of ${MAX_TOTAL_DRAFT_SCRIPT_BYTES}`,
    })
  }
  return issues
}

function validateMount(mount: unknown, path: string, issues: EdgeFunctionValidationIssue[]): void {
  if (typeof mount !== 'object' || mount === null || Array.isArray(mount)) {
    issues.push({ field: path, code: 'invalid_field', message: 'mount must be an object' })
    return
  }
  const record = mount as {
    projects?: { include?: unknown; exclude?: unknown }
    priority?: unknown
    ratio?: unknown
  }
  const projects = record.projects
  if (typeof projects !== 'object' || projects === null || Array.isArray(projects)) {
    issues.push({
      field: `${path}.projects`,
      code: 'invalid_field',
      message: 'mount projects must be an object',
    })
    return
  }
  for (const [listName, list, minimum] of [
    ['include', projects.include, 1],
    ['exclude', projects.exclude ?? [], 0],
  ] as const) {
    if (!Array.isArray(list)) {
      issues.push({
        field: `${path}.projects.${listName}`,
        code: 'invalid_field',
        message: `projects.${listName} must be an array`,
      })
      continue
    }
    if (list.length > MAX_GLOBS_PER_SELECTOR) {
      issues.push({
        field: `${path}.projects.${listName}`,
        code: 'too_many_entries',
        message: `projects.${listName} may hold at most ${MAX_GLOBS_PER_SELECTOR} entries`,
      })
    }
    if (list.length < minimum) {
      issues.push({
        field: `${path}.projects.${listName}`,
        code: 'missing_field',
        message: `projects.${listName} needs at least ${minimum} entr${minimum === 1 ? 'y' : 'ies'}`,
      })
    }
    list.forEach((glob, globIndex) => {
      if (typeof glob !== 'string' || !isGlobCandidate(glob)) {
        issues.push({
          field: `${path}.projects.${listName}[${globIndex}]`,
          code: 'invalid_glob',
          message: 'mount globs must be printable, non-empty, without ";"',
        })
      }
    })
  }
  const priority = record.priority ?? 0
  if (
    typeof priority !== 'number' ||
    !Number.isSafeInteger(priority) ||
    priority < EDGE_FUNCTION_PRIORITY_MIN ||
    priority > EDGE_FUNCTION_PRIORITY_MAX
  ) {
    issues.push({
      field: `${path}.priority`,
      code: 'invalid_field',
      message: `priority must be an integer in [${EDGE_FUNCTION_PRIORITY_MIN}, ${EDGE_FUNCTION_PRIORITY_MAX}]`,
    })
  }
  const ratio = record.ratio ?? 10_000
  if (
    typeof ratio !== 'number' ||
    !Number.isSafeInteger(ratio) ||
    ratio < EDGE_FUNCTION_RATIO_MIN ||
    ratio > EDGE_FUNCTION_RATIO_MAX
  ) {
    issues.push({
      field: `${path}.ratio`,
      code: 'invalid_field',
      message: `ratio must be an integer in [${EDGE_FUNCTION_RATIO_MIN}, ${EDGE_FUNCTION_RATIO_MAX}]`,
    })
  }
}
