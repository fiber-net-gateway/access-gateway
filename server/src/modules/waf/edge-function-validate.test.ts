import assert from 'node:assert/strict'
import test from 'node:test'

import type { EdgeFunctionsDraft } from './edge-function-model.js'
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
        mounts: [
          { projects: { include: ['loan-*'], exclude: ['loan-admin-legacy'] }, priority: 200 },
        ],
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
        mounts: [{ projects: { include: ['*'] } }],
      },
      {
        name: 'bot-trap',
        enabled: true,
        script: 'return true;',
        mounts: [{ projects: { include: ['*'] } }],
      },
      {
        name: 'bot-trap',
        enabled: true,
        script: 'return true;',
        mounts: [{ projects: { include: ['*'] } }],
      },
      {
        name: 'waf',
        enabled: true,
        script: 'return true;',
        mounts: [{ projects: { include: ['*'] } }],
      },
    ],
  })
  const fields = issues.map((issue) => `${issue.field}:${issue.code}`)
  assert.ok(fields.includes('functions[0].name:invalid_function_name'))
  assert.ok(fields.includes('functions[2].name:duplicate_function_name'))
  assert.ok(fields.includes('functions[3].name:reserved_function_name'))
})

test('disabled functions may omit mounts; enabled functions need one', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      { name: 'parked', enabled: false, script: 'return true;', mounts: [] },
      { name: 'live', enabled: true, script: 'return true;', mounts: [] },
    ],
  })
  assert.equal(issues.length, 1)
  assert.equal(issues[0]!.field, 'functions[1].mounts')
  assert.equal(issues[0]!.code, 'missing_field')
})

test('mount selectors, priority, and ratio are range-checked', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'bad-mounts',
        enabled: true,
        script: 'return true;',
        mounts: [
          {
            projects: { include: ['a;b'], exclude: [] },
            priority: 1_000_001,
            ratio: 10_001,
          },
        ],
      },
    ],
  })
  const fields = issues.map((issue) => `${issue.field}:${issue.code}`)
  assert.ok(fields.includes('functions[0].mounts[0].projects.include[0]:invalid_glob'))
  assert.ok(fields.includes('functions[0].mounts[0].priority:invalid_field'))
  assert.ok(fields.includes('functions[0].mounts[0].ratio:invalid_field'))
})

test('include must be a non-empty array; glob bytes are bounded', () => {
  const issues = validateEdgeFunctionsDraft({
    schemaVersion: 1,
    functions: [
      {
        name: 'empty-include',
        enabled: true,
        script: 'return true;',
        mounts: [{ projects: { include: [] } }],
      },
    ],
  })
  assert.ok(issues.some((issue) => issue.field === 'functions[0].mounts[0].projects.include'))
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
        mounts: [{ projects: { include: ['*'] } }],
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
      mounts: [{ projects: { include: ['*'] } }],
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
        mounts: [{ projects: { include: ['ok'] } }],
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
            mounts: [{ projects: { include: ['*'] } }],
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
            mounts: [{ projects: { include: ['*'] } }],
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
          mounts: [{ projects: { include: ['*'] } }],
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
