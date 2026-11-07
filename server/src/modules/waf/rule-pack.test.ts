import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import test from 'node:test'

import { compileWafPolicy } from './generator.js'
import type { WafGroup, WafPolicy, WafRule } from './model.js'
import {
  WAF_RULE_PACK,
  WAF_RULE_PACK_GROUPS,
  WAF_RULE_PACK_VERSION,
  wafRulePackView,
} from './rule-pack.js'
import { validateWafPolicy } from './validate.js'

const ruleIdPattern = /^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/u

test('rule pack ids are stable wire-safe slugs without duplicates', () => {
  const ids = WAF_RULE_PACK.map((rule) => rule.id)
  assert.equal(new Set(ids).size, ids.length)
  for (const id of ids) {
    assert.match(id, ruleIdPattern)
  }
})

test('rule pack rules carry curated actions and stay in the request-head phase', () => {
  for (const rule of WAF_RULE_PACK) {
    assert.equal(rule.phase, 'request_head')
    assert.ok(rule.action === 'log' || rule.action === 'block')
    assert.ok(
      rule.severity === 'critical' ||
        rule.severity === 'high' ||
        rule.severity === 'medium' ||
        rule.severity === 'low',
    )
    assert.ok(rule.name.length > 0)
  }
  // Noisy families stay log-only; at least one high-signal family blocks.
  const actions = new Set(WAF_RULE_PACK.map((rule) => rule.action))
  assert.ok(actions.has('log'))
  assert.ok(actions.has('block'))
})

test('rule pack view exposes the version and the exact rule data', () => {
  const view = wafRulePackView()
  assert.equal(view.version, WAF_RULE_PACK_VERSION)
  assert.equal(view.groups, WAF_RULE_PACK_GROUPS)
  assert.equal(view.rules, WAF_RULE_PACK)
})

test('rule pack groups follow the families and every group holds rules', () => {
  const ids = WAF_RULE_PACK_GROUPS.map((group) => group.id)
  assert.equal(new Set(ids).size, ids.length)
  assert.deepEqual([...new Set(WAF_RULE_PACK.map((rule) => rule.group))].sort(), [...ids].sort())
})

test('the rule pack imports into a fresh draft without changing anything', () => {
  const policy: WafPolicy = {
    schemaVersion: 2,
    groups: [...WAF_RULE_PACK_GROUPS],
    rules: [...WAF_RULE_PACK],
    projectModes: {},
  }
  // Every pack group is off: the pack validates cleanly and no function is
  // mounted — nothing changes until an operator turns a group on (灰度顺序
  // 单项目 log → block).
  assert.deepEqual(validateWafPolicy(policy), [])
  const unmounted = compileWafPolicy(policy)
  assert.equal(unmounted.kind, 'unmount')
  assert.equal(unmounted.reason, 'no-mounted-groups')

  // With log enabled, group order keeps the pack's rule priority order.
  const logging: WafPolicy = {
    ...policy,
    groups: policy.groups.map((group) => ({ ...group, mode: 'log' })),
  }
  const compilation = compileWafPolicy(logging)
  assert.equal(compilation.kind, 'mounted')
  assert.deepEqual(
    compilation.functions.flatMap((fn) => fn.ruleOrder),
    [...WAF_RULE_PACK].sort((a, b) => a.priority - b.priority).map((rule) => rule.id),
  )
})

test('the committed golden fixture pins the live rule pack data', async () => {
  const fixturePath = fileURLToPath(new URL('./fixtures/rule-pack-v2.policy.json', import.meta.url))
  const fixture = JSON.parse(await readFile(fixturePath, 'utf8')) as {
    groups: WafGroup[]
    rules: WafRule[]
  }
  assert.deepEqual(fixture.rules, WAF_RULE_PACK)
  assert.deepEqual(
    fixture.groups.map((group) => ({ ...group, mode: 'off' })),
    WAF_RULE_PACK_GROUPS,
  )
})
