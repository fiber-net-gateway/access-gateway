import assert from 'node:assert/strict'
import { existsSync } from 'node:fs'
import { readFile, readdir } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { fileURLToPath } from 'node:url'
import test from 'node:test'

import { compileWafPolicy, locateScriptOffset } from './generator.js'
import type { WafPolicy, WafRule } from './model.js'
import { WAF_FUNCTION_NAME, WAF_MOUNT_PRIORITY } from './model.js'
import { validateWafPolicy } from './validate.js'

function policy(overrides: Partial<WafPolicy> = {}): WafPolicy {
  return {
    schemaVersion: 1,
    defaultMode: 'log',
    rules: [],
    projectModes: {},
    ...overrides,
  }
}

function rule(overrides: Partial<WafRule> = {}): WafRule {
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

function compileOrThrow(input: WafPolicy): ReturnType<typeof compileWafPolicy> {
  const issues = validateWafPolicy(input)
  assert.deepEqual(issues, [])
  return compileWafPolicy(input)
}

const fixturesDirectory = fileURLToPath(new URL('./fixtures/', import.meta.url))

test('golden fixture policies compile to their committed scripts byte-for-byte', async () => {
  const names = (await readdir(fixturesDirectory))
    .filter((name) => name.endsWith('.policy.json'))
    .map((name) => name.slice(0, -'.policy.json'.length))
  assert.ok(names.length >= 3)

  for (const name of names) {
    const input = JSON.parse(
      await readFile(`${fixturesDirectory}${name}.policy.json`, 'utf8'),
    ) as WafPolicy
    assert.deepEqual(validateWafPolicy(input), [], `${name} fixture must be valid`)
    const result = compileWafPolicy(input)
    assert.equal(result.kind, 'mounted', `${name} fixture must mount`)
    if (result.kind !== 'mounted') {
      continue
    }
    const golden = await readFile(`${fixturesDirectory}${name}.script.txt`, 'utf8')
    assert.equal(result.script, golden, `${name} script drifted from its golden snapshot`)
    assert.equal(
      result.scriptSha256,
      createHash('sha256').update(Buffer.from(golden, 'utf8')).digest('hex'),
      `${name} sha256 must be the hash of the golden bytes`,
    )
    assert.equal(result.functionName, WAF_FUNCTION_NAME)
    assert.equal(result.mountPriority, WAF_MOUNT_PRIORITY)
  }
})

test('mixed fixture: shared cidr directive, priority order with declaration ties, mount excludes off', async () => {
  const input = JSON.parse(
    await readFile(`${fixturesDirectory}mixed.policy.json`, 'utf8'),
  ) as WafPolicy
  const result = compileOrThrow(input)
  assert.equal(result.kind, 'mounted')
  if (result.kind !== 'mounted') {
    return
  }
  assert.deepEqual(result.ruleOrder, ['scanner-ua', 'sqli-0023', 'path-traversal'])
  // Both rules' identical CIDR sets (authored in different orders) share net1.
  assert.deepEqual(result.directives, [
    { name: 'net1', cidrs: ['10.164.44.2/32', '172.28.0.0/16'] },
  ])
  assert.equal(result.script.match(/directive net1/gu)?.length, 1)
  assert.deepEqual(result.mounts, [{ include: ['*'], exclude: ['loan-admin-legacy'] }])
})

test('allowlist fixture: default-mode off mounts only explicitly enabled projects', async () => {
  const input = JSON.parse(
    await readFile(`${fixturesDirectory}allowlist-default-off.policy.json`, 'utf8'),
  ) as WafPolicy
  const result = compileOrThrow(input)
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.deepEqual(result.mounts, [{ include: ['loan-admin', 'trade-web'], exclude: [] }])
    assert.ok(result.script.includes('$ctx.project == "trade-web"'))
  }
})

test('empty rules compile to an unmount candidate', () => {
  assert.deepEqual(compileOrThrow(policy()), { kind: 'unmount', reason: 'no-rules' })
})

test('default-mode off with no enabled projects compiles to an unmount candidate', () => {
  const result = compileOrThrow(
    policy({ defaultMode: 'off', projectModes: { retired: 'off' }, rules: [rule()] }),
  )
  assert.deepEqual(result, { kind: 'unmount', reason: 'no-mounted-projects' })
})

test('action=log rules report without any mode gate', () => {
  const result = compileOrThrow(
    policy({
      defaultMode: 'block',
      projectModes: { 'loan-admin': 'log' },
      rules: [rule({ action: 'log' })],
    }),
  )
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.ok(result.script.includes('ctx.report("waf.log", "probe-0001");'))
    assert.ok(!result.script.includes('waf.block'))
    assert.ok(!result.script.includes('resp.sendJson'))
  }
})

test('block rule with no log-mode projects emits a flat block branch', () => {
  const result = compileOrThrow(policy({ defaultMode: 'block', rules: [rule()] }))
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.ok(result.script.includes('ctx.report("waf.block", "probe-0001");'))
    assert.ok(result.script.includes('resp.sendJson(403,'))
    assert.ok(!result.script.includes('} else {'))
  }
})

test('block rule with no reachable block projects degrades to log-only', () => {
  const result = compileOrThrow(
    policy({ defaultMode: 'log', projectModes: { 'loan-admin': 'log' }, rules: [rule()] }),
  )
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.ok(result.script.includes('ctx.report("waf.log", "probe-0001");'))
    assert.ok(!result.script.includes('resp.sendJson'))
  }
})

test('default-mode log with block projects gates on the block enumeration', () => {
  const result = compileOrThrow(
    policy({
      defaultMode: 'log',
      projectModes: { alpha: 'block', beta: 'block', gamma: 'log' },
      rules: [rule()],
    }),
  )
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.ok(
      result.script.includes(
        'if (($ctx.project == "alpha" || $ctx.project == "beta")) {\n        ctx.report("waf.block"',
      ),
    )
    assert.ok(result.script.includes('} else {\n        ctx.report("waf.log"'))
  }
})

test('scope chains fold into prefix/equality chains before the match expression', () => {
  const result = compileOrThrow(
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
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    const condition = /if \((.*)\) \{/u.exec(result.script)?.[1] ?? ''
    const parts = condition.split(' && ')
    assert.equal(parts.length, 4)
    assert.equal(parts[0] ?? '', '($req.method == "POST" || $req.method == "GET")')
    assert.equal(parts[1] ?? '', '(strings.hasPrefix($req.path, "/api/") || $req.path == "/exact")')
    assert.equal(parts[2] ?? '', 'net1.contains($conn.remote_addr)')
    assert.ok((parts[3] ?? '').startsWith('('))
    assert.ok((parts[3] ?? '').endsWith(')'))
  }
})

test('exclusions become project inequality guards inside the rule condition', () => {
  const result = compileOrThrow(
    policy({
      rules: [rule({ exclusions: [{ project: 'loan-web', reason: 'reports' }] })],
    }),
  )
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.ok(result.script.includes('if ($ctx.project != "loan-web" && '))
  }
})

test('multi-line match expressions keep their line breaks inside parentheses', () => {
  const result = compileOrThrow(
    policy({
      rules: [rule({ match: 'strings.match($req.uri, "a")\n  || strings.match($req.uri, "b")' })],
    }),
  )
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.ok(
      result.script.includes(
        '(\nstrings.match($req.uri, "a")\n  || strings.match($req.uri, "b")\n)',
      ),
    )
  }
})

test('project names with string metacharacters are escaped in generated code', () => {
  const result = compileOrThrow(
    policy({
      defaultMode: 'block',
      projectModes: { 'a"b': 'log', 'c\\d': 'log' },
      rules: [rule()],
    }),
  )
  assert.equal(result.kind, 'mounted')
  if (result.kind === 'mounted') {
    assert.ok(result.script.includes('!($ctx.project == "a\\x22b" || $ctx.project == "c\\x5cd")'))
  }
})

test('compilation is deterministic and CIDR order does not change the script hash', () => {
  const first = compileOrThrow(
    policy({ rules: [rule({ scope: { sourceCidrs: ['10.0.0.0/8', '172.28.0.0/16'] } })] }),
  )
  const second = compileOrThrow(
    policy({ rules: [rule({ scope: { sourceCidrs: ['172.28.0.0/16', '10.0.0.0/8'] } })] }),
  )
  assert.equal(first.kind, 'mounted')
  assert.equal(second.kind, 'mounted')
  if (first.kind === 'mounted' && second.kind === 'mounted') {
    assert.equal(first.script, second.script)
    assert.equal(first.scriptSha256, second.scriptSha256)
  }
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
    const names = (await readdir(fixturesDirectory)).filter((name) => name.endsWith('.policy.json'))
    for (const name of names) {
      const input = JSON.parse(await readFile(`${fixturesDirectory}${name}`, 'utf8')) as WafPolicy
      const result = compileWafPolicy(input)
      assert.equal(result.kind, 'mounted', name)
      if (result.kind !== 'mounted') {
        continue
      }
      const request = JSON.stringify({
        contractVersion: 1,
        requestId: '00000000-0000-4000-8000-000000000002',
        kind: 'edge_function_script',
        project: '',
        payloadBase64: Buffer.from(result.script, 'utf8').toString('base64'),
      })
      const done = spawnSync(validatorPath, {
        input: request,
        maxBuffer: 1 << 20,
        encoding: 'utf8',
      })
      const response = JSON.parse(done.stdout) as { valid: boolean; errors?: unknown[] }
      assert.equal(response.valid, true, `${name}: ${JSON.stringify(response.errors)}`)
    }
  },
)

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
        exclusions: [{ project: 'api.example.com', reason: 'false positive' }],
      }),
      rule({ id: 'multi-line', priority: 300, match: '$req.path == "/a"\n  && $req.path == "/b"' }),
    ],
  })
  const result = compileOrThrow(input)
  assert.equal(result.kind, 'mounted')
  if (result.kind !== 'mounted') {
    return
  }

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

  // The match text is found behind multi-byte names, exclusions and scope.
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
  assert.deepEqual(locateScriptOffset(input, result, byteOffset('$ctx.project != ')), {
    ruleIndex: 1,
  })
  assert.equal(locateScriptOffset(input, result, 0), null)
})
