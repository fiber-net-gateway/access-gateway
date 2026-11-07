import assert from 'node:assert/strict'
import test from 'node:test'

import type { WafPolicy } from './model.js'
import { upgradeWafPolicy } from './policy-upgrade.js'
import { validateWafPolicy } from './validate.js'

function schemaOneRule(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    id: 'probe-0001',
    name: 'probe',
    severity: 'high',
    priority: 100,
    action: 'block',
    phase: 'request_head',
    match: 'strings.contains($req.path, "../")',
    ...overrides,
  }
}

function upgrade(stored: Record<string, unknown>): WafPolicy {
  const policy = upgradeWafPolicy({ schemaVersion: 1, projectModes: {}, ...stored }) as WafPolicy
  assert.deepEqual(validateWafPolicy(policy), [])
  return policy
}

test('schema 2 policies pass through untouched', () => {
  const current = { schemaVersion: 2, groups: [], rules: [], projectModes: {} }
  assert.equal(upgradeWafPolicy(current), current)
})

test('a fresh schema 1 draft upgrades to an empty one, keeping project modes', () => {
  assert.deepEqual(
    upgrade({ defaultMode: 'off', rules: [], projectModes: { 'loan-admin': 'log' } }),
    { schemaVersion: 2, groups: [], rules: [], projectModes: { 'loan-admin': 'log' } },
  )
})

test('rules move into one default group carrying the environment mode', () => {
  const policy = upgrade({
    defaultMode: 'block',
    rules: [schemaOneRule()],
    projectModes: { 'loan-admin': 'log', legacy: 'off' },
  })
  assert.deepEqual(policy.groups, [
    {
      id: 'default',
      name: '默认规则组',
      mode: 'block',
      priority: 100,
      projects: { include: ['*'] },
    },
  ])
  assert.equal(policy.rules[0]?.group, 'default')
  assert.deepEqual(policy.projectModes, { 'loan-admin': 'log', legacy: 'off' })
})

test('default off with enabled projects scopes the group to exactly those', () => {
  const policy = upgrade({
    defaultMode: 'off',
    rules: [schemaOneRule()],
    projectModes: { 'trade-web': 'block', 'loan-admin': 'log', legacy: 'off' },
  })
  assert.equal(policy.groups[0]?.mode, 'log')
  assert.deepEqual(policy.groups[0]?.projects.include, ['loan-admin', 'trade-web'])
})

test('default off with nothing enabled stays off', () => {
  const policy = upgrade({ defaultMode: 'off', rules: [schemaOneRule()] })
  assert.equal(policy.groups[0]?.mode, 'off')
})

test('per-rule exclusions become guards in the rule match', () => {
  const policy = upgrade({
    defaultMode: 'log',
    rules: [
      schemaOneRule({
        match: '  $req.path == "/a"  ',
        exclusions: [
          { project: 'loan-web', reason: 'reports' },
          { project: 'a"b', reason: 'quotes' },
        ],
      }),
      schemaOneRule({
        id: 'multi-line',
        match: '$req.path == "/a"\n  || $req.path == "/b"',
        exclusions: [{ project: 'loan-web', reason: 'reports' }],
      }),
    ],
  })
  assert.equal(
    policy.rules[0]?.match,
    '$ctx.project != "loan-web" && $ctx.project != "a\\x22b" && ($req.path == "/a")',
  )
  assert.equal(
    policy.rules[1]?.match,
    '$ctx.project != "loan-web" && (\n$req.path == "/a"\n  || $req.path == "/b"\n)',
  )
  assert.ok(policy.rules.every((rule) => !('exclusions' in rule)))
})

test('a malformed schema 1 document is left for validation to reject', () => {
  const stored = { schemaVersion: 1, defaultMode: 'log', rules: 'nope', projectModes: {} }
  assert.equal(upgradeWafPolicy(stored), stored)
  assert.notDeepEqual(validateWafPolicy(stored as unknown as WafPolicy), [])
})
