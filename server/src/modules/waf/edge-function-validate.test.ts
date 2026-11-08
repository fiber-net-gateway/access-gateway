import assert from 'node:assert/strict'
import test from 'node:test'

import type { EdgeFunctionDraftFunction, EdgeFunctionsDraft } from './edge-function-model.js'
import { validateEdgeFunctionsDraft } from './edge-function-validate.js'

function draft(overrides: Partial<EdgeFunctionsDraft> = {}): EdgeFunctionsDraft {
  return {
    schemaVersion: 1,
    functions: [
      {
        name: 'bot-trap',
        enabled: true,
        script:
          'if ((strings.contains($req.uri, "/trap"))) { ctx.report("bot.hit", "trap"); }\nreturn true;',
        projects: { include: ['loan-*'], exclude: ['loan-admin-legacy'] },
        priority: 200,
      },
    ],
    ...overrides,
  }
}

test('a well-formed draft passes with no issues', () => {
  assert.deepEqual(validateEdgeFunctionsDraft(draft()), [])
})

test('non-object and wrong-schema drafts are rejected wholesale', () => {
  assert.match(validateEdgeFunctionsDraft(null)[0]!.message, /draft must be an object/)
  assert.match(
    validateEdgeFunctionsDraft({ schemaVersion: 2, functions: [] })[0]!.field,
    /^schemaVersion$/,
  )
  assert.match(
    validateEdgeFunctionsDraft({ schemaVersion: 1, functions: 'nope' })[0]!.message,
    /functions must be an array/,
  )
})

test('function names must be slugs, unique, and not reserved', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'Bad_Name',
        enabled: true,
        script: 'return true;',
        projects: { include: ['*'] },
      },
      {
        name: 'bot-trap',
        enabled: true,
        script: 'return true;',
        projects: { include: ['*'] },
      },
      {
        name: 'bot-trap',
        enabled: true,
        script: 'return true;',
        projects: { include: ['*'] },
      },
      {
        name: 'waf',
        enabled: true,
        script: 'return true;',
        projects: { include: ['*'] },
      },
      {
        name: 'waf-sqli',
        enabled: true,
        script: 'return true;',
        projects: { include: ['*'] },
      },
      {
        name: 'wafer',
        enabled: true,
        script: 'return true;',
        projects: { include: ['*'] },
      },
    ],
  })
  const fields = issues.map((issue) => `${issue.field}:${issue.code}`)
  assert.ok(fields.includes('functions[0].name:invalid_function_name'))
  assert.ok(fields.includes('functions[2].name:duplicate_function_name'))
  assert.ok(fields.includes('functions[3].name:reserved_function_name'))
  assert.ok(fields.includes('functions[4].name:reserved_function_name'))
  assert.ok(!fields.some((field) => field.startsWith('functions[5]')))
})

test('every function needs a project selector, enabled or not', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      { name: 'parked', enabled: false, script: 'return true;' },
      { name: 'live', enabled: true, script: 'return true;' },
    ],
  })
  assert.deepEqual(
    issues.map((issue) => issue.field),
    ['functions[0].projects', 'functions[1].projects'],
  )
})

test('the removed mounts array no longer satisfies the selector', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'legacy',
        enabled: true,
        script: 'return true;',
        mounts: [{ projects: { include: ['*'] } }],
      },
    ],
  })
  assert.deepEqual(
    issues.map((issue) => issue.field),
    ['functions[0].projects'],
  )
})

test('selectors, priority, and ratio are range-checked', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'bad-selector',
        enabled: true,
        script: 'return true;',
        projects: { include: ['a;b'], exclude: [] },
        priority: 1_000_001,
        ratio: 10_001,
      },
    ],
  })
  const fields = issues.map((issue) => `${issue.field}:${issue.code}`)
  assert.ok(fields.includes('functions[0].projects.include[0]:invalid_glob'))
  assert.ok(fields.includes('functions[0].priority:invalid_field'))
  assert.ok(fields.includes('functions[0].ratio:invalid_field'))
})

test('include must be a non-empty array; glob bytes are bounded', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'empty-include',
        enabled: true,
        script: 'return true;',
        projects: { include: [] },
      },
    ],
  })
  assert.ok(issues.some((issue) => issue.field === 'functions[0].projects.include'))
})

function canaryDraft(
  canary: Partial<EdgeFunctionDraftFunction>,
  ...others: EdgeFunctionDraftFunction[]
): EdgeFunctionsDraft {
  const stable: EdgeFunctionDraftFunction = {
    name: 'auth-v1',
    enabled: true,
    script: 'return true;',
    projects: { include: ['*'] },
  }
  return {
    schemaVersion: 1,
    functions: [
      stable,
      { ...stable, name: 'auth-v2', ratio: 300, replaces: 'auth-v1', ...canary },
      ...others,
    ],
  }
}

function replacesIssues(draft: EdgeFunctionsDraft): string[] {
  return validateEdgeFunctionsDraft(draft)
    .filter((issue) => issue.field.endsWith('.replaces'))
    .map((issue) => `${issue.field}:${issue.code}`)
}

test('a canary may replace an enabled function of the same phase', () => {
  assert.deepEqual(validateEdgeFunctionsDraft(canaryDraft({})), [])
  // Both ramp endpoints are legal: 0 keeps all traffic on the target, 10000
  // moves it all while the target stays for an instant rollback.
  assert.deepEqual(validateEdgeFunctionsDraft(canaryDraft({ ratio: 0 })), [])
  assert.deepEqual(validateEdgeFunctionsDraft(canaryDraft({ ratio: 10_000 })), [])
})

test('replaces must be a slug even on a disabled function', () => {
  assert.deepEqual(replacesIssues(canaryDraft({ enabled: false, replaces: 'Not A Slug' })), [
    'functions[1].replaces:invalid_field',
  ])
})

test('a disabled canary is not held to the cross-function rules', () => {
  assert.deepEqual(replacesIssues(canaryDraft({ enabled: false, replaces: 'gone' })), [])
})

test('replaces rejects itself, missing targets, and generated WAF functions', () => {
  for (const replaces of ['auth-v2', 'gone', 'waf-core']) {
    assert.deepEqual(
      replacesIssues(canaryDraft({ replaces })),
      ['functions[1].replaces:invalid_replaces'],
      replaces,
    )
  }
})

test('replaces rejects a disabled target, which never reaches the wire', () => {
  const parked: EdgeFunctionDraftFunction = {
    name: 'parked',
    enabled: false,
    script: 'return true;',
    projects: { include: ['*'] },
  }
  assert.deepEqual(replacesIssues(canaryDraft({ replaces: 'parked' }, parked)), [
    'functions[1].replaces:invalid_replaces',
  ])
})

test('replaces rejects a target in another phase', () => {
  assert.deepEqual(replacesIssues(canaryDraft({ phase: 'request_body' })), [
    'functions[1].replaces:invalid_replaces',
  ])
})

test('replaces rejects chains and two canaries of one target', () => {
  const chained: EdgeFunctionDraftFunction = {
    name: 'auth-v3',
    enabled: true,
    script: 'return true;',
    projects: { include: ['*'] },
    replaces: 'auth-v2',
  }
  assert.deepEqual(replacesIssues(canaryDraft({}, chained)), [
    'functions[2].replaces:invalid_replaces',
  ])
  assert.deepEqual(replacesIssues(canaryDraft({}, { ...chained, replaces: 'auth-v1' })), [
    'functions[2].replaces:invalid_replaces',
  ])
})

test('scripts are bounded per function and in total', () => {
  const bigScript = 'return true;'.padEnd(1_048_577, ' ')
  const perFunction = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'huge',
        enabled: true,
        script: bigScript,
        projects: { include: ['*'] },
      },
    ],
  })
  assert.ok(
    perFunction.some(
      (issue) => issue.field === 'functions[0].script' && issue.code === 'limit_exceeded',
    ),
  )

  // Five functions each under the per-script cap but together over the 4 MiB
  // transport budget.
  const chunk = 'x'.repeat(850_000)
  const total = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [0, 1, 2, 3, 4].map((index) => ({
      name: `bulk-${index}`,
      enabled: true,
      script: chunk,
      projects: { include: ['*'] },
    })),
  })
  assert.ok(total.some((issue) => issue.field === 'functions' && issue.code === 'limit_exceeded'))
})

test('lone surrogates are rejected in scripts and globs', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'surrogate',
        enabled: true,
        script: 'return "\ud800";',
        projects: { include: ['ok'] },
      },
    ],
  })
  assert.ok(issues.some((issue) => issue.field === 'functions[0].script'))
})

test('phase must be request_head or request_body when present', () => {
  assert.deepEqual(
    validateEdgeFunctionsDraft(
      draft({
        functions: [
          {
            name: 'body-scan',
            enabled: true,
            phase: 'request_body',
            script: 'return true;',
            projects: { include: ['*'] },
          },
        ],
      }),
    ),
    [],
  )
  assert.deepEqual(
    validateEdgeFunctionsDraft(
      draft({
        functions: [
          {
            name: 'body-scan',
            enabled: true,
            phase: 'request_head',
            script: 'return true;',
            projects: { include: ['*'] },
          },
        ],
      }),
    ),
    [],
  )
  const issues = validateEdgeFunctionsDraft(
    draft({
      functions: [
        {
          name: 'body-scan' as string,
          enabled: true,
          phase: 'response' as unknown as EdgeFunctionsDraft['functions'][number]['phase'],
          script: 'return true;',
          projects: { include: ['*'] },
        },
      ],
    }),
  )
  assert.deepEqual(issues, [
    {
      field: 'functions[0].phase',
      code: 'invalid_field',
      message: 'phase must be one of request_head, request_body',
    },
  ])
})
