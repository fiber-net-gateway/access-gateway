import assert from 'node:assert/strict'
import test from 'node:test'

import {
  groupModeSets,
  groupMountSelector,
  projectGlobMatches,
  projectInGroup,
} from './group-scope.js'
import type { WafGroup, WafPolicy } from './model.js'

// The EdgeGlob.MatchesSingleStarSemantics cases from
// native/access-server/tests/EdgeFunctionMountTableTest.cpp: a drift here makes
// mode gates disagree with the mount the data plane expands.
test('project globs follow the native single-star semantics', () => {
  for (const [pattern, project] of [
    ['*', 'anything'],
    ['*', ''],
    ['loan-*', 'loan-'],
    ['loan-*', 'loan-web-2'],
    ['a*b*c', 'aXbYc'],
    ['a*b*c', 'abc'],
    ['', ''],
  ] as const) {
    assert.equal(projectGlobMatches(pattern, project), true, `${pattern} ~ ${project}`)
  }
  for (const [pattern, project] of [
    ['loan-*', 'api'],
    ['loan-*', 'xloan-web'],
    ['a*b*c', 'abx'],
    ['Loan-*', 'loan-web'],
    ['loan.?', 'loan-x'],
    ['', 'x'],
  ] as const) {
    assert.equal(projectGlobMatches(pattern, project), false, `${pattern} !~ ${project}`)
  }
})

function group(projects: WafGroup['projects']): WafGroup {
  return { id: 'core', name: 'core', mode: 'log', priority: 100, projects }
}

test('an exemption vetoes include', () => {
  const scoped = group({
    include: ['loan-*'],
    exempt: [{ project: 'loan-web', reason: 'reports' }],
  })
  assert.equal(projectInGroup(scoped, 'loan-api'), true)
  assert.equal(projectInGroup(scoped, 'loan-web'), false)
  assert.equal(projectInGroup(scoped, 'trade-web'), false)
})

test('the mount merges exemptions with off-mode members and stays sorted', () => {
  const scoped = group({
    include: ['trade-*', 'loan-*'],
    exempt: [{ project: 'loan-web', reason: 'reports' }],
  })
  const policy: WafPolicy = {
    schemaVersion: 2,
    groups: [scoped],
    rules: [],
    projectModes: {
      'loan-api': 'off',
      'loan-web': 'off', // already exempt: not a member, no duplicate entry
      'other-site': 'off', // outside the group
      'trade-web': 'block',
    },
  }
  const sets = groupModeSets(policy, scoped)
  assert.deepEqual(sets, { off: ['loan-api'], log: [], block: ['trade-web'] })
  assert.deepEqual(groupMountSelector(scoped, sets), {
    include: ['loan-*', 'trade-*'],
    exclude: ['loan-api', 'loan-web'],
  })
})
