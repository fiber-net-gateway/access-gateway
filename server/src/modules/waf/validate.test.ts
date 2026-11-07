import assert from 'node:assert/strict'
import test from 'node:test'

import type { WafGroup, WafPolicy } from './model.js'
import { isValidCidrLiteral, isValidProjectName, validateWafPolicy } from './validate.js'

function group(overrides: Partial<WafGroup> = {}): WafGroup {
  return {
    id: 'core',
    name: 'core rules',
    mode: 'log',
    priority: 100,
    projects: { include: ['*'] },
    ...overrides,
  }
}

function basePolicy(overrides: Partial<WafPolicy> = {}): WafPolicy {
  return {
    schemaVersion: 2,
    groups: [group()],
    rules: [],
    projectModes: {},
    ...overrides,
  }
}

function rule(overrides: Partial<WafPolicy['rules'][number]>): WafPolicy['rules'][number] {
  return {
    id: 'sqli-0001',
    name: 'probe',
    group: 'core',
    severity: 'high',
    priority: 100,
    action: 'block',
    phase: 'request_head',
    match: 'strings.contains($req.path, "../")',
    ...overrides,
  }
}

function issueFields(policy: WafPolicy): string[] {
  return validateWafPolicy(policy).map((issue) => issue.field)
}

test('valid fixture policies produce no issues', () => {
  const fixture: WafPolicy = basePolicy({
    groups: [
      group({
        mode: 'block',
        projects: {
          include: ['loan-*', 'trade-web'],
          exempt: [{ project: 'loan-web', reason: 'report export contains SQL fragments' }],
        },
      }),
    ],
    projectModes: { 'loan-admin': 'log', 'loan-admin-legacy': 'off' },
    rules: [
      rule({
        scope: { methods: ['GET', 'POST'], paths: ['/api/*'], sourceCidrs: ['172.28.0.0/16'] },
      }),
    ],
  })
  assert.deepEqual(validateWafPolicy(fixture), [])
})

test('schema envelope fields are validated', () => {
  assert.deepEqual(issueFields(basePolicy({ schemaVersion: 1 as WafPolicy['schemaVersion'] })), [
    'schemaVersion',
  ])
})

test('rule identity and metadata fields are validated', () => {
  for (const bad of ['SQLI', 'sqli_1', '-sqli', 'sqli-', 'a'.repeat(65), '']) {
    assert.ok(
      issueFields(basePolicy({ rules: [rule({ id: bad })] })).includes('rules[0].id'),
      `id: ${bad}`,
    )
  }
  assert.ok(
    issueFields(basePolicy({ rules: [rule({ name: 'line\nbreak' })] })).includes('rules[0].name'),
  )
  assert.ok(issueFields(basePolicy({ rules: [rule({ name: '' })] })).includes('rules[0].name'))
  assert.ok(
    issueFields(basePolicy({ rules: [rule({ severity: 'extreme' as never })] })).includes(
      'rules[0].severity',
    ),
  )
  for (const bad of [-1, 1.5, 1_000_001]) {
    assert.ok(
      issueFields(basePolicy({ rules: [rule({ priority: bad })] })).includes('rules[0].priority'),
      `priority: ${String(bad)}`,
    )
  }
  assert.ok(
    issueFields(basePolicy({ rules: [rule({ action: 'deny' as never })] })).includes(
      'rules[0].action',
    ),
  )
  assert.ok(
    issueFields(basePolicy({ rules: [rule({ phase: 'response' as never })] })).includes(
      'rules[0].phase',
    ),
  )
})

test('match expression shape is validated', () => {
  assert.ok(issueFields(basePolicy({ rules: [rule({ match: '   ' })] })).includes('rules[0].match'))
  assert.ok(
    issueFields(basePolicy({ rules: [rule({ match: 'x'.repeat(8_193) })] })).includes(
      'rules[0].match',
    ),
  )
  const loneSurrogate = `strings.contains($req.path, "${String.fromCharCode(0xd800)}")`
  assert.ok(
    issueFields(basePolicy({ rules: [rule({ match: loneSurrogate })] })).includes('rules[0].match'),
  )
})

test('scope methods, paths, and CIDRs are validated', () => {
  const issues = issueFields(
    basePolicy({
      rules: [
        rule({
          scope: {
            methods: ['get', 'GET'],
            paths: ['api/*', '/a*b', '/dup', '/dup', '*'],
            sourceCidrs: [
              '10.0.0.0/8',
              '10.0.0.0/8',
              '300.0.0.1/8',
              '10.0.0.0/40',
              'noslash',
              '10.0.0.0/',
            ],
          },
        }),
      ],
    }),
  )
  assert.deepEqual(
    issues.filter((field) => field === 'rules[0].scope.methods'),
    ['rules[0].scope.methods'],
  )
  assert.ok(issues.filter((field) => field === 'rules[0].scope.paths').length >= 4)
  assert.ok(issues.filter((field) => field === 'rules[0].scope.sourceCidrs').length >= 5)
})

test('group identity and metadata fields are validated', () => {
  for (const bad of ['SQLI', 'sqli_1', '-sqli', 'sqli-', 'a'.repeat(61), '']) {
    assert.ok(
      issueFields(basePolicy({ groups: [group({ id: bad })] })).includes('groups[0].id'),
      `id: ${bad}`,
    )
  }
  // 60 bytes plus the waf- prefix is exactly the 64-byte function name cap.
  assert.deepEqual(issueFields(basePolicy({ groups: [group({ id: 'a'.repeat(60) })] })), [])
  assert.ok(issueFields(basePolicy({ groups: [group({ name: '  ' })] })).includes('groups[0].name'))
  assert.ok(
    issueFields(basePolicy({ groups: [group({ mode: 'warn' as never })] })).includes(
      'groups[0].mode',
    ),
  )
  for (const bad of [-1, 1.5, 1_000_001]) {
    assert.ok(
      issueFields(basePolicy({ groups: [group({ priority: bad })] })).includes(
        'groups[0].priority',
      ),
      `priority: ${String(bad)}`,
    )
  }
})

test('a group needs a project selector made of valid, distinct globs', () => {
  const include = (values: string[]) =>
    issueFields(basePolicy({ groups: [group({ projects: { include: values } })] }))
  assert.deepEqual(include([]), ['groups[0].projects.include'])
  assert.deepEqual(include(['loan;web']), ['groups[0].projects.include'])
  assert.deepEqual(include(['loan-*', 'loan-*']), ['groups[0].projects.include'])
  assert.deepEqual(include(Array.from({ length: 257 }, (_, index) => `p${index}`)), [
    'groups[0].projects.include',
  ])
})

test('exemptions require a printable project and a non-blank reason', () => {
  const issues = issueFields(
    basePolicy({
      groups: [
        group({
          projects: {
            include: ['*'],
            exempt: [
              { project: 'loan;web', reason: '' },
              { project: 'loan-*', reason: 'ok'.repeat(200) },
              { project: 'trade-web', reason: 'line\nbreak' },
            ],
          },
        }),
      ],
    }),
  )
  assert.deepEqual(issues, [
    'groups[0].projects.exempt[0].project',
    'groups[0].projects.exempt[0].reason',
    'groups[0].projects.exempt[1].reason',
    'groups[0].projects.exempt[2].reason',
  ])
})

test('duplicate ids and duplicate exemptions conflict', () => {
  assert.deepEqual(issueFields(basePolicy({ rules: [rule({}), rule({ id: 'sqli-0001' })] })), [
    'rules[1].id',
  ])
  assert.deepEqual(issueFields(basePolicy({ groups: [group(), group()] })), ['groups[1].id'])
  const issues = issueFields(
    basePolicy({
      groups: [
        group({
          projects: {
            include: ['*'],
            exempt: [
              { project: 'loan-web', reason: 'a' },
              { project: 'loan-web', reason: 'b' },
            ],
          },
        }),
      ],
    }),
  )
  assert.deepEqual(issues, ['groups[0].projects.exempt[1].project'])
})

test('a rule must belong to an existing group', () => {
  const issues = validateWafPolicy(basePolicy({ rules: [rule({ group: 'missing' })] }))
  assert.deepEqual(
    issues.map(({ field, code }) => [field, code]),
    [['rules[0].group', 'unknown_group']],
  )
})

test('the wire exclude cap counts exemptions together with off-mode members', () => {
  const exempt = Array.from({ length: 200 }, (_, index) => ({
    project: `exempt-${index}`,
    reason: 'audit',
  }))
  const projectModes = Object.fromEntries(
    Array.from({ length: 57 }, (_, index) => [`off-${index}`, 'off' as const]),
  )
  const issues = validateWafPolicy(
    basePolicy({ groups: [group({ projects: { include: ['*'], exempt } })], projectModes }),
  )
  assert.deepEqual(
    issues.map(({ field, code }) => [field, code]),
    [['groups[0].projects.exempt', 'limit_exceeded']],
  )
  // Off-mode projects outside the group never reach its exclude list.
  assert.deepEqual(
    issueFields(
      basePolicy({ groups: [group({ projects: { include: ['loan-*'], exempt } })], projectModes }),
    ),
    [],
  )
})

test('projectModes keys and values are validated', () => {
  const issues = issueFields(
    basePolicy({ projectModes: { 'loan;web': 'log', 'loan-web': 'warn' as never } }),
  )
  assert.ok(issues.includes("projectModes['loan;web']"))
  assert.ok(issues.includes("projectModes['loan-web']"))
})

test('group, rule and projectModes counts are bounded', () => {
  const manyGroups = Array.from({ length: 33 }, (_, index) => group({ id: `g${index}` }))
  assert.deepEqual(issueFields(basePolicy({ groups: manyGroups })), ['groups'])

  const manyRules = Array.from({ length: 257 }, () => rule({}))
  assert.deepEqual(issueFields(basePolicy({ rules: manyRules })), ['rules'])

  const manyModes: Record<string, WafPolicy['projectModes'][string]> = {}
  for (let index = 0; index < 1_025; index += 1) {
    manyModes[`p${index}`] = 'log'
  }
  assert.deepEqual(issueFields(basePolicy({ projectModes: manyModes })), ['projectModes'])
})

test('isValidProjectName rejects wire-hostile project names', () => {
  assert.ok(isValidProjectName('loan-web'))
  assert.ok(isValidProjectName('trade.web.2'))
  assert.ok(!isValidProjectName(''))
  assert.ok(!isValidProjectName('loan web'))
  assert.ok(!isValidProjectName('loan;web'))
  assert.ok(!isValidProjectName('loan*web'))
  assert.ok(!isValidProjectName('a'.repeat(256)))
})

test('isValidCidrLiteral accepts sane literals and rejects typos', () => {
  assert.ok(isValidCidrLiteral('10.0.0.0/8'))
  assert.ok(isValidCidrLiteral('172.28.1.5/32'))
  assert.ok(isValidCidrLiteral('2001:db8::/32'))
  assert.ok(isValidCidrLiteral('::1/128'))
  assert.ok(!isValidCidrLiteral('10.0.0.0'))
  assert.ok(!isValidCidrLiteral('10.0.0.0/'))
  assert.ok(!isValidCidrLiteral('/8'))
  assert.ok(!isValidCidrLiteral('10.0.0.0/33'))
  assert.ok(!isValidCidrLiteral('256.0.0.0/8'))
  assert.ok(!isValidCidrLiteral('010.0.0.0/8'))
  assert.ok(!isValidCidrLiteral('2001:db8::/129'))
  assert.ok(!isValidCidrLiteral('2001::db8::/32'))
})
