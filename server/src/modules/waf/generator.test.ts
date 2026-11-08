import assert from 'node:assert/strict'
import { existsSync } from 'node:fs'
import { readFile, readdir } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { fileURLToPath } from 'node:url'
import test from 'node:test'

import {
  compileWafPolicy,
  locateScriptOffset,
  type WafCompilation,
  type WafFunctionCompilation,
} from './generator.js'
import type { WafGroup, WafPolicy, WafRule } from './model.js'
import { validateWafPolicy } from './validate.js'

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

function policy(overrides: Partial<WafPolicy> = {}): WafPolicy {
  return {
    schemaVersion: 2,
    groups: [group()],
    rules: [],
    projectModes: {},
    ...overrides,
  }
}

function rule(overrides: Partial<WafRule> = {}): WafRule {
  return {
    id: 'probe-0001',
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

function compileOrThrow(input: WafPolicy): WafCompilation {
  const issues = validateWafPolicy(input)
  assert.deepEqual(issues, [])
  return compileWafPolicy(input)
}

function mountedFunctions(input: WafPolicy): readonly WafFunctionCompilation[] {
  const result = compileOrThrow(input)
  assert.equal(result.kind, 'mounted')
  return result.kind === 'mounted' ? result.functions : []
}

function onlyFunction(input: WafPolicy): WafFunctionCompilation {
  const functions = mountedFunctions(input)
  assert.equal(functions.length, 1)
  return functions[0]!
}

const fixturesDirectory = fileURLToPath(new URL('./fixtures/', import.meta.url))

async function fixturePolicy(name: string): Promise<WafPolicy> {
  return JSON.parse(await readFile(`${fixturesDirectory}${name}.policy.json`, 'utf8')) as WafPolicy
}

test('golden fixture policies compile to their committed scripts byte-for-byte', async () => {
  const files = await readdir(fixturesDirectory)
  const names = files
    .filter((name) => name.endsWith('.policy.json'))
    .map((name) => name.slice(0, -'.policy.json'.length))
  assert.ok(names.length >= 3)

  for (const name of names) {
    const input = await fixturePolicy(name)
    assert.deepEqual(validateWafPolicy(input), [], `${name} fixture must be valid`)
    const functions = mountedFunctions(input)
    assert.deepEqual(
      functions.map((fn) => `${name}.${fn.functionName}.script.txt`).sort(),
      files.filter((file) => file.startsWith(`${name}.`) && file.endsWith('.script.txt')).sort(),
      `${name} goldens must match its functions exactly`,
    )
    for (const fn of functions) {
      const golden = await readFile(
        `${fixturesDirectory}${name}.${fn.functionName}.script.txt`,
        'utf8',
      )
      assert.equal(fn.script, golden, `${name} ${fn.functionName} drifted from its golden snapshot`)
      assert.equal(
        fn.scriptSha256,
        createHash('sha256').update(Buffer.from(golden, 'utf8')).digest('hex'),
        `${name} sha256 must be the hash of the golden bytes`,
      )
      assert.equal(fn.functionName, `waf-${fn.groupId}`)
    }
  }
})

test('mixed fixture: one function per group, ordered by group priority, selectors carry the scope', async () => {
  const functions = mountedFunctions(await fixturePolicy('mixed'))
  assert.deepEqual(
    functions.map((fn) => [fn.functionName, fn.priority, fn.ruleOrder]),
    [
      ['waf-scanner', 50, ['scanner-ua']],
      ['waf-core', 100, ['sqli-0023', 'path-traversal']],
    ],
  )
  const [scanner, core] = functions
  assert.deepEqual(scanner?.projects, {
    include: ['loan-*', 'trade-*'],
    exclude: ['loan-admin-legacy'],
  })
  // Exemptions and off-mode projects land in the same wire exclude list.
  assert.deepEqual(core?.projects, { include: ['*'], exclude: ['loan-admin-legacy', 'loan-web'] })
  // Both rules' identical CIDR sets (authored in different orders) share net1.
  assert.deepEqual(core?.directives, [{ name: 'net1', cidrs: ['10.164.44.2/32', '172.28.0.0/16'] }])
  assert.equal(core?.script.match(/directive net1/gu)?.length, 1)
})

test('allowlist fixture: a group mounts on exactly the projects it lists', async () => {
  const fn = onlyFunction(await fixturePolicy('allowlist'))
  assert.deepEqual(fn.projects, { include: ['loan-admin', 'trade-web'], exclude: [] })
  assert.ok(fn.script.includes('$ctx.project == "trade-web"'))
})

test('empty rules compile to an unmount candidate', () => {
  assert.deepEqual(compileOrThrow(policy()), {
    kind: 'unmount',
    reason: 'no-rules',
    skipped: [{ groupId: 'core', reason: 'no-rules' }],
  })
})

test('rules in off groups compile to an unmount candidate', () => {
  const result = compileOrThrow(policy({ groups: [group({ mode: 'off' })], rules: [rule()] }))
  assert.deepEqual(result, {
    kind: 'unmount',
    reason: 'no-mounted-groups',
    skipped: [{ groupId: 'core', reason: 'off' }],
  })
})

test('off and empty groups are skipped while the rest still publish', () => {
  const result = compileOrThrow(
    policy({
      groups: [group(), group({ id: 'idle' }), group({ id: 'paused', mode: 'off' })],
      rules: [rule(), rule({ id: 'paused-rule', group: 'paused' })],
    }),
  )
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.deepEqual(
      result.functions.map((fn) => fn.functionName),
      ['waf-core'],
    )
    assert.deepEqual(result.skipped, [
      { groupId: 'idle', reason: 'no-rules' },
      { groupId: 'paused', reason: 'off' },
    ])
  }
})

test('groups with equal priority order by id', () => {
  const functions = mountedFunctions(
    policy({
      groups: [group({ id: 'zeta' }), group({ id: 'alpha' })],
      rules: [rule({ id: 'z', group: 'zeta' }), rule({ id: 'a', group: 'alpha' })],
    }),
  )
  assert.deepEqual(
    functions.map((fn) => fn.groupId),
    ['alpha', 'zeta'],
  )
})

test('action=log rules report without any mode gate', () => {
  const { script } = onlyFunction(
    policy({
      groups: [group({ mode: 'block' })],
      projectModes: { 'loan-admin': 'log' },
      rules: [rule({ action: 'log' })],
    }),
  )
  assert.ok(script.includes('ctx.report("waf.log", "probe-0001");'))
  assert.ok(!script.includes('waf.block'))
  assert.ok(!script.includes('resp.sendJson'))
})

test('block rule with no log-mode projects emits a flat block branch', () => {
  const { script } = onlyFunction(policy({ groups: [group({ mode: 'block' })], rules: [rule()] }))
  assert.ok(script.includes('ctx.report("waf.block", "probe-0001");'))
  assert.ok(script.includes('resp.sendJson(403,'))
  assert.ok(!script.includes('} else {'))
})

test('a block stops the script so one request is blocked once', () => {
  const { script } = onlyFunction(
    policy({
      groups: [group({ mode: 'block' })],
      rules: [rule(), rule({ id: 'probe-0002', priority: 200 })],
    }),
  )
  assert.ok(
    script.includes(
      'resp.sendJson(403, {code: "WAF_BLOCKED", ruleId: "probe-0001"});\n    return true;\n}',
    ),
  )
})

test('block rule with no reachable block projects degrades to log-only', () => {
  const { script } = onlyFunction(
    policy({ projectModes: { 'loan-admin': 'log' }, rules: [rule()] }),
  )
  assert.ok(script.includes('ctx.report("waf.log", "probe-0001");'))
  assert.ok(!script.includes('resp.sendJson'))
})

test('group mode log with block projects gates on the block enumeration', () => {
  const { script } = onlyFunction(
    policy({
      projectModes: { alpha: 'block', beta: 'block', gamma: 'log' },
      rules: [rule()],
    }),
  )
  assert.ok(
    script.includes(
      'if (($ctx.project == "alpha" || $ctx.project == "beta")) {\n        ctx.report("waf.block"',
    ),
  )
  assert.ok(script.includes('} else {\n        ctx.report("waf.log"'))
})

test('a project override only reaches the groups the project belongs to', () => {
  const groups = [
    group({ id: 'loan', projects: { include: ['loan-*'] } }),
    group({ id: 'trade', projects: { include: ['trade-*'] } }),
    group({
      id: 'partial',
      projects: { include: ['*'], exempt: [{ project: 'loan-web', reason: 'false positive' }] },
    }),
  ]
  const rules = [
    rule({ id: 'loan-rule', group: 'loan' }),
    rule({ id: 'trade-rule', group: 'trade' }),
    rule({ id: 'partial-rule', group: 'partial' }),
  ]
  const before = mountedFunctions(policy({ groups, rules }))
  const after = mountedFunctions(
    policy({ groups, rules, projectModes: { 'loan-web': 'block', 'loan-api': 'off' } }),
  )
  const byGroup = (id: string, functions: readonly WafFunctionCompilation[]) =>
    functions.find((fn) => fn.groupId === id)!

  assert.equal(byGroup('trade', after).scriptSha256, byGroup('trade', before).scriptSha256)
  assert.deepEqual(byGroup('trade', after).projects, byGroup('trade', before).projects)
  assert.notEqual(byGroup('loan', after).scriptSha256, byGroup('loan', before).scriptSha256)
  assert.deepEqual(byGroup('loan', after).projects, { include: ['loan-*'], exclude: ['loan-api'] })
  // loan-web is exempt from this group, so its block override must not show up.
  assert.ok(!byGroup('partial', after).script.includes('loan-web'))
  assert.deepEqual(byGroup('partial', after).projects, {
    include: ['*'],
    exclude: ['loan-api', 'loan-web'],
  })
})

test('scope chains fold into prefix/equality chains before the match expression', () => {
  const { script } = onlyFunction(
    policy({
      rules: [
        rule({
          match: 'strings.match($req.uri, "a") || strings.match($req.uri, "b")',
          scope: {
            methods: ['POST', 'GET'],
            paths: ['/api/*', '/exact'],
            sourceCidrs: ['10.0.0.0/8'],
          },
        }),
      ],
    }),
  )
  const condition = /if \((.*)\) \{/u.exec(script)?.[1] ?? ''
  const parts = condition.split(' && ')
  assert.equal(parts.length, 4)
  assert.equal(parts[0] ?? '', '($req.method == "POST" || $req.method == "GET")')
  assert.equal(parts[1] ?? '', '(strings.hasPrefix($req.path, "/api/") || $req.path == "/exact")')
  assert.equal(parts[2] ?? '', 'net1.contains($conn.remote_addr)')
  assert.ok((parts[3] ?? '').startsWith('('))
  assert.ok((parts[3] ?? '').endsWith(')'))
})

test('multi-line match expressions keep their line breaks inside parentheses', () => {
  const { script } = onlyFunction(
    policy({
      rules: [rule({ match: 'strings.match($req.uri, "a")\n  || strings.match($req.uri, "b")' })],
    }),
  )
  assert.ok(
    script.includes('(\nstrings.match($req.uri, "a")\n  || strings.match($req.uri, "b")\n)'),
  )
})

test('project names with string metacharacters are escaped in generated code', () => {
  const { script } = onlyFunction(
    policy({
      groups: [group({ mode: 'block' })],
      projectModes: { 'a"b': 'log', 'c\\d': 'log' },
      rules: [rule()],
    }),
  )
  assert.ok(script.includes('!($ctx.project == "a\\x22b" || $ctx.project == "c\\x5cd")'))
})

test('compilation is deterministic and CIDR order does not change the script hash', () => {
  const first = onlyFunction(
    policy({ rules: [rule({ scope: { sourceCidrs: ['10.0.0.0/8', '172.28.0.0/16'] } })] }),
  )
  const second = onlyFunction(
    policy({ rules: [rule({ scope: { sourceCidrs: ['172.28.0.0/16', '10.0.0.0/8'] } })] }),
  )
  assert.equal(first.script, second.script)
  assert.equal(first.scriptSha256, second.scriptSha256)
})

test('a group display name stays out of the script bytes', () => {
  const rules = [rule()]
  assert.equal(
    onlyFunction(policy({ groups: [group({ name: 'before' })], rules })).scriptSha256,
    onlyFunction(policy({ groups: [group({ name: 'after' })], rules })).scriptSha256,
  )
})

const validatorCandidates = [
  process.env.NATIVE_VALIDATOR_PATH,
  fileURLToPath(new URL('../../../../native/build/apps/access-gateway-validator', import.meta.url)),
]
const validatorPath = validatorCandidates.find((candidate) => candidate && existsSync(candidate))

test(
  'every golden script compiles with the real native validator',
  { skip: validatorPath ? false : 'native validator binary not built' },
  async () => {
    assert.ok(validatorPath)
    const names = (await readdir(fixturesDirectory))
      .filter((name) => name.endsWith('.policy.json'))
      .map((name) => name.slice(0, -'.policy.json'.length))
    for (const name of names) {
      for (const fn of mountedFunctions(await fixturePolicy(name))) {
        const request = JSON.stringify({
          contractVersion: 1,
          requestId: '00000000-0000-4000-8000-000000000002',
          kind: 'edge_function_script',
          project: '',
          payloadBase64: Buffer.from(fn.script, 'utf8').toString('base64'),
        })
        const done = spawnSync(validatorPath, {
          input: request,
          maxBuffer: 1 << 20,
          encoding: 'utf8',
        })
        const response = JSON.parse(done.stdout) as { valid: boolean; errors?: unknown[] }
        assert.equal(
          response.valid,
          true,
          `${name} ${fn.functionName}: ${JSON.stringify(response.errors)}`,
        )
      }
    }
  },
)

test('rule spans index the whole policy, whichever group a rule compiled into', () => {
  const input = policy({
    groups: [group(), group({ id: 'other' })],
    rules: [rule(), rule({ id: 'second', group: 'other' }), rule({ id: 'third', group: 'other' })],
  })
  const other = mountedFunctions(input).find((fn) => fn.groupId === 'other')!
  assert.deepEqual(
    other.ruleSpans.map((span) => span.ruleIndex),
    [1, 2],
  )
})

test('rule spans locate each rule block and its match text by script byte offset', () => {
  const input = policy({
    rules: [
      // Declared first but executes last: spans keep the declaration index.
      rule({ id: 'late-rule', priority: 200, name: '中文名称', match: '$req.path == "/late"' }),
      rule({
        id: 'early-rule',
        priority: 10,
        match: '  strings.contains($req.path, "/中文")  ',
        scope: { methods: ['GET', 'POST'] },
      }),
      rule({ id: 'multi-line', priority: 300, match: '$req.path == "/a"\n  && $req.path == "/b"' }),
    ],
  })
  const result = onlyFunction(input)

  const script = Buffer.from(result.script, 'utf8')
  const byteOffset = (text: string): number => script.indexOf(Buffer.from(text, 'utf8'))

  assert.deepEqual(
    result.ruleSpans.map((span) => span.ruleIndex),
    [1, 0, 2],
  )
  for (const span of result.ruleSpans) {
    const block = script.subarray(span.start, span.end).toString('utf8')
    assert.ok(block.startsWith(`// ${input.rules[span.ruleIndex]!.id} (priority`))
    assert.ok(block.endsWith('}\n'))
  }

  // The match text is found behind multi-byte names and scope.
  assert.equal(result.ruleSpans[0]!.matchStart, byteOffset('strings.contains($req.path, "/中文")'))
  assert.equal(result.ruleSpans[1]!.matchStart, byteOffset('$req.path == "/late"'))
  assert.equal(result.ruleSpans[2]!.matchStart, byteOffset('$req.path == "/a"'))

  // Positions count from the authored text: a single-line match keeps its
  // trimmed leading spaces in the column, and columns are characters, not bytes.
  const earlyMatch = input.rules[1]!.match
  assert.deepEqual(locateScriptOffset(input, result, byteOffset('文') + 3), {
    ruleIndex: 1,
    match: { line: 1, column: earlyMatch.indexOf('文') + 2 },
  })
  assert.deepEqual(locateScriptOffset(input, result, byteOffset('&& $req.path == "/b"')), {
    ruleIndex: 2,
    match: { line: 2, column: 3 },
  })

  // An unbalanced expression fails on the generated `{`: clamp to the match end.
  const lateMatch = input.rules[0]!.match
  const openingBrace = result.ruleSpans[1]!.matchStart + Buffer.byteLength(lateMatch) + 3
  assert.equal(script.subarray(openingBrace, openingBrace + 1).toString('utf8'), '{')
  assert.deepEqual(locateScriptOffset(input, result, openingBrace), {
    ruleIndex: 0,
    match: { line: 1, column: lateMatch.length + 1 },
  })

  // Generated guards belong to the rule but not to its match text, and the
  // script header belongs to no rule at all.
  assert.deepEqual(locateScriptOffset(input, result, byteOffset('$req.method')), {
    ruleIndex: 1,
  })
  assert.equal(locateScriptOffset(input, result, 0), null)
})
