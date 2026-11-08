import { cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, test, vi } from 'vitest'
import { createMemoryRouter } from 'react-router'
import { RouterProvider } from 'react-router/dom'

import type { WafPolicy } from '../api/types'
import { appRoutes } from '../router'

vi.mock('../components/YamlCodeEditor', () => ({
  YamlCodeEditor: ({
    ariaLabel,
    value,
    onChange,
    onSave,
  }: {
    ariaLabel: string
    value: string
    onChange(value: string): void
    onSave(): void
  }) => (
    <textarea
      aria-label={ariaLabel}
      value={value}
      onChange={(event) => onChange(event.target.value)}
      onKeyDown={(event) => {
        if ((event.ctrlKey || event.metaKey) && event.key === 's') {
          event.preventDefault()
          onSave()
        }
      }}
    />
  ),
}))

const projectId = '00000000-0000-4000-8000-000000000001'
const releaseId = '00000000-0000-4000-8000-00000000000c'

const policy = {
  schemaVersion: 2,
  groups: [
    {
      id: 'core',
      name: '核心规则',
      mode: 'log',
      priority: 100,
      projects: {
        include: ['*'],
        exempt: [{ project: 'api.example.com', reason: '误报待修' }],
      },
    },
    {
      id: 'loan',
      name: '贷款规则',
      mode: 'block',
      priority: 50,
      projects: { include: ['loan-*'] },
    },
  ],
  rules: [
    {
      id: 'probe-0001',
      name: '路径穿越探测',
      group: 'core',
      severity: 'high',
      priority: 100,
      action: 'block',
      phase: 'request_head',
      match: 'strings.contains($req.path, "../")',
    },
  ],
  projectModes: { 'legacy.example.com': 'off' },
}

const policyDraft = {
  id: '00000000-0000-4000-8000-00000000000b',
  policy,
  policySha256: '0'.repeat(64),
  lockVersion: '3',
  createdAt: '2026-11-06T00:00:00.000Z',
  updatedAt: '2026-11-06T00:00:00.000Z',
}

const compilePreview = {
  valid: true,
  issues: [],
  compilation: {
    kind: 'mounted',
    functions: [
      {
        groupId: 'core',
        functionName: 'waf-core',
        script: 'function waf() {\n  return 1\n}\n',
        scriptSha256: 'a'.repeat(64),
        priority: 100,
        projects: { include: ['*'], exclude: ['api.example.com', 'legacy.example.com'] },
        ruleOrder: ['probe-0001'],
        directives: [],
      },
    ],
    skipped: [{ groupId: 'loan', reason: 'no-rules' }],
  },
  scriptValid: true,
  scriptErrors: [],
  validator: { contractVersion: 1, revision: 'test' },
}

const releaseView = {
  id: releaseId,
  sequence: '5',
  status: 'ready',
  wafFunctions: [{ name: 'waf-core', scriptSha256: '1'.repeat(64) }],
  mode: 'mounted',
  ruleCount: 1,
  indexVersion: '5',
  description: '',
  resources: [
    {
      id: '00000000-0000-4000-8000-000000000010',
      kind: 'edge_function_script',
      dataId: 'ploto.unified-access.edge-function.waf-core',
      group: 'ACCESS-SERVER',
      status: 'verified',
      verifiedSha256: '1'.repeat(64),
      verifiedAt: '2026-11-07T00:00:00.000Z',
    },
    {
      id: '00000000-0000-4000-8000-000000000011',
      kind: 'edge_function_index',
      dataId: 'ploto.unified-access.edge-functions',
      group: 'ACCESS-SERVER',
      status: 'verified',
      verifiedSha256: '2'.repeat(64),
      verifiedAt: '2026-11-07T00:00:00.000Z',
    },
  ],
  publication: { jobId: null, state: null },
  activationStatus: 'unknown',
  activation: {
    status: 'unknown',
    targetCount: 0,
    activeCount: 0,
    pendingCount: 0,
    degradedCount: 0,
    unknownCount: 0,
    evaluatedAt: null,
  },
  createdAt: '2026-11-07T00:00:00.000Z',
  publishedAt: null,
}

const edgeFunctions = {
  index: {
    exists: true,
    sha256: '3'.repeat(64),
    md5: '3'.repeat(32),
    version: 5,
    functions: [
      {
        name: 'waf-core',
        sha256: '1'.repeat(64),
        phase: null,
        projects: { include: ['*'], exclude: ['api.example.com', 'legacy.example.com'] },
        priority: 100,
        ratio: null,
        replaces: null,
      },
      {
        name: 'old-fn',
        sha256: '9'.repeat(64),
        phase: 'request_body',
        projects: { include: ['*'], exclude: [] },
        priority: 500,
        ratio: null,
        replaces: null,
      },
    ],
    parseError: null,
  },
  scripts: [
    {
      name: 'waf-core',
      exists: true,
      sha256: '1'.repeat(64),
      content: 'function waf() {\n  return 0\n}\n',
    },
  ],
}

const edgeFunctionsDraft = {
  id: '00000000-0000-4000-8000-00000000000e',
  draft: {
    schemaVersion: 1,
    functions: [
      {
        name: 'bot-trap',
        description: '抓扫描器',
        enabled: true,
        script:
          'if (($header.user_agent contains "zgrab")) { ctx.report("bot.hit", "zgrab"); }\nreturn true;',
        projects: { include: ['loan-*'], exclude: ['loan-admin-legacy'] },
        priority: 200,
        ratio: 5000,
      },
    ],
  },
  draftSha256: '4'.repeat(64),
  lockVersion: '2',
  createdAt: '2026-11-07T00:00:00.000Z',
  updatedAt: '2026-11-07T00:00:00.000Z',
}

const edgeCompilePreview = {
  valid: true,
  issues: [],
  functions: [
    {
      name: 'bot-trap',
      phase: 'request_head',
      scriptSha256: 'b'.repeat(64),
      scriptValid: true,
      errors: [],
    },
  ],
  validator: { contractVersion: 1, revision: 'test' },
}

const rulePack = {
  version: 'waf-rule-pack-v2',
  groups: [
    { id: 'sqli', name: 'SQL 注入', mode: 'off', priority: 100, projects: { include: ['*'] } },
    // Only holds a rule that already exists, so it must not be added.
    { id: 'unused', name: '已有规则', mode: 'off', priority: 300, projects: { include: ['*'] } },
  ],
  rules: [
    { ...policy.rules[0], group: 'unused' }, // same id as the draft rule → skipped on import
    {
      id: 'sqli-union-select',
      name: 'union select 注入探测',
      group: 'sqli',
      severity: 'critical',
      priority: 100,
      action: 'block',
      phase: 'request_head',
      match: 'strings.match($req.uri, "(?i)union(?:[+%20]|\\\\s)+select")',
    },
  ],
}

const wafHits = {
  window: '24h',
  queriedAt: '2026-11-07T02:00:00.000Z',
  rows: [
    { ruleId: 'probe-0001', event: 'waf.log', count: 4 },
    { ruleId: 'probe-0001', event: 'waf.block', count: 2 },
    { ruleId: 'retired-rule', event: 'waf.block', count: 9 },
  ],
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  })
}

function installApiMock() {
  const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url
    if (url === '/api/health') {
      return jsonResponse({
        status: 'ok',
        service: 'access-gateway-console-api',
        version: '0.1.0',
      })
    }
    if (url === '/api/system/status') {
      return jsonResponse({
        status: 'ready',
        service: 'access-gateway-console-api',
        dependencies: {
          database: { status: 'ready', detail: 'ready' },
          schema: { status: 'ready', detail: 'ready' },
          authentication: { status: 'ready', detail: 'ready' },
          nativeValidator: {
            status: 'ready',
            detail: 'ready',
            contractVersion: 1,
            revision: 'test',
            limits: null,
          },
          publicationWorker: { status: 'ready', detail: 'ready' },
          activationCollector: { status: 'ready', detail: 'ready' },
        },
      })
    }
    if (url === '/api/waf/policy' && (!init?.method || init.method === 'GET')) {
      return jsonResponse(policyDraft)
    }
    if (url === '/api/waf/rule-pack') {
      return jsonResponse(rulePack)
    }
    if (url === '/api/waf/hits?window=24h') {
      return jsonResponse(wafHits)
    }
    if (url.startsWith('/api/waf/hits')) {
      const requested = new URL(url, 'http://localhost').searchParams.get('window') ?? '1h'
      return jsonResponse({ ...wafHits, window: requested })
    }
    if (url === '/api/waf/policy' && init?.method === 'PUT') {
      const body = JSON.parse(String(init.body)) as { policy: typeof policy }
      return jsonResponse({ ...policyDraft, policy: body.policy, lockVersion: '4' })
    }
    if (url === '/api/waf/policy/compile-preview' && init?.method === 'POST') {
      return jsonResponse(compilePreview)
    }
    if (url === '/api/waf/releases' && (!init?.method || init.method === 'GET')) {
      return jsonResponse({ items: [releaseView] })
    }
    if (url === `/api/waf/releases/${releaseId}/publication` && init?.method === 'POST') {
      return jsonResponse(
        {
          jobId: '00000000-0000-4000-8000-00000000000d',
          state: 'queued',
          release: { ...releaseView, status: 'queued' },
        },
        200,
      )
    }
    if (url === `/api/waf/releases/${releaseId}/rollback` && init?.method === 'POST') {
      return jsonResponse(
        {
          ...releaseView,
          sequence: '6',
          description: 'rollback of WAF edge functions #5',
        },
        201,
      )
    }
    if (url === '/api/waf/edge-functions/draft' && (!init?.method || init.method === 'GET')) {
      return jsonResponse(edgeFunctionsDraft)
    }
    if (url === '/api/waf/edge-functions/draft' && init?.method === 'PUT') {
      const body = JSON.parse(String(init.body)) as { draft: typeof edgeFunctionsDraft.draft }
      return jsonResponse({ ...edgeFunctionsDraft, draft: body.draft, lockVersion: '3' })
    }
    if (url === '/api/waf/edge-functions/compile-preview' && init?.method === 'POST') {
      return jsonResponse(edgeCompilePreview)
    }
    if (url === '/api/waf/edge-functions') {
      return jsonResponse(edgeFunctions)
    }
    if (url === `/api/projects/${projectId}`) {
      return jsonResponse({
        id: projectId,
        domain: 'api.example.com',
        status: 'active',
        lockVersion: '4',
        draft: { id: 'draft-id', state: 'editing', revision: 8, lockVersion: '8' },
        publishedVersion: null,
        activationStatus: 'unknown',
        createdAt: '2026-08-01T00:00:00.000Z',
        updatedAt: '2026-08-12T00:00:00.000Z',
      })
    }
    if (url === `/api/projects/${projectId}/configuration-versions/current`) {
      return jsonResponse({
        version: {
          id: '00000000-0000-4000-8000-000000000002',
          projectId,
          number: 8,
          relation: 'current',
          baseVersionId: null,
          restoredFromVersionId: null,
          changeSummary: 'Initial routes',
          routeCount: 0,
          modelSha256: 'sha256',
          validationState: 'valid',
          publicationStatus: 'never',
          createdBy: { id: 'user-id', displayName: 'Test User' },
          createdAt: '2026-08-11T00:00:00.000Z',
          model: {
            schemaVersion: 6,
            kind: 'project_routes_yaml',
            hostAliases: [],
            networkPolicy: {
              source: 'route',
              httpsRedirect: 'off',
              allowedCidrs: [],
              deniedCidrs: [],
            },
            routes: [],
          },
        },
        lockVersion: '8',
      })
    }
    return jsonResponse({ error: { message: `Unhandled test URL: ${url}` } }, 404)
  })
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

async function openPolicyPage(awaitPreview = true) {
  const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf'] })
  const user = userEvent.setup()
  render(<RouterProvider router={router} />)
  expect(await screen.findByRole('heading', { name: 'WAF 策略' })).toBeTruthy()
  if (awaitPreview) expect(await screen.findByText(/将发布 1 个边缘函数/u)).toBeTruthy()
  return user
}

type MockedFetch = ReturnType<typeof installApiMock>

function savedPolicyBody(fetchMock: MockedFetch) {
  const call = fetchMock.mock.calls.find(
    ([url, init]) => url === '/api/waf/policy' && init?.method === 'PUT',
  )
  if (!call) return undefined
  return (
    JSON.parse(String(call[1]?.body)) as {
      policy: {
        groups: {
          id: string
          mode: string
          projects: { include: string[]; exempt?: unknown }
        }[]
        rules: {
          name: string
          group: string
          scope?: unknown
        }[]
      }
    }
  ).policy
}

function previewBodies(fetchMock: MockedFetch) {
  return fetchMock.mock.calls
    .filter(([url]) => url === '/api/waf/policy/compile-preview')
    .map(
      ([, init]) =>
        JSON.parse(String(init?.body)) as {
          groups: { id: string; mode: string }[]
          rules: { name: string }[]
        },
    )
}

describe('WAF console pages', () => {
  test('edits the policy draft and saves it with optimistic concurrency', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: 'WAF 策略' })).toBeTruthy()
    expect(screen.getByText('路径穿越探测')).toBeTruthy()
    expect(screen.getByText(/probe-0001 · strings\.contains/u)).toBeTruthy()

    // Configured rules stay compact: no editor is mounted until one is opened.
    expect(screen.queryByRole('textbox', { name: 'match 表达式' })).toBeNull()

    // The debounced compile preview renders the mounted compilation summary.
    expect(await screen.findByText(/将发布 1 个边缘函数/u)).toBeTruthy()
    expect(screen.getByText('不发布：贷款规则（没有规则）')).toBeTruthy()
    expect(screen.getByText(/waf-core · 1 条规则 · 脚本 \d+ 行/u)).toBeTruthy()

    // Importing the starter rule pack skips the existing id and appends the rest
    // into the dirty working copy.
    await user.click(screen.getByRole('button', { name: '导入起手规则包' }))
    expect(
      await screen.findByText(/已导入 1 条规则、1 个规则组（waf-rule-pack-v2），跳过已存在 1 条/u),
    ).toBeTruthy()
    expect(await screen.findByText('union select 注入探测')).toBeTruthy()
    expect(screen.getByRole('region', { name: '规则组 SQL 注入' })).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '编辑规则组 核心规则' }))
    const dialog = await screen.findByRole('dialog', { name: '编辑规则组' })
    await user.selectOptions(within(dialog).getByRole('combobox', { name: '模式' }), 'off')
    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    await user.click(screen.getByRole('button', { name: '保存策略草稿' }))

    await waitFor(() => {
      const saveCall = fetchMock.mock.calls.find(
        ([url, init]) => url === '/api/waf/policy' && init?.method === 'PUT',
      )
      expect(saveCall).toBeTruthy()
      expect(new Headers(saveCall?.[1]?.headers).get('If-Match')).toBe('"3"')
      const body = JSON.parse(String(saveCall?.[1]?.body)) as { policy: typeof policy }
      expect(body.policy.groups.map((group) => [group.id, group.mode])).toEqual([
        ['core', 'off'],
        ['loan', 'block'],
        ['sqli', 'off'],
      ])
      expect(body.policy.groups[0]?.projects.exempt).toEqual([
        { project: 'api.example.com', reason: '误报待修' },
      ])
      expect(body.policy.rules.length).toBe(2)
      expect(body.policy.rules[1]?.id).toBe('sqli-union-select')
      expect(body.policy.rules[1]?.group).toBe('sqli')
    })
  })

  test('edits a rule in a dialog, keeping typed lists and marking the row as changed', async () => {
    const fetchMock = installApiMock()
    const user = await openPolicyPage()

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    const dialog = await screen.findByRole('dialog', { name: '编辑规则' })

    // Project scope belongs to the group: the rule dialog has no exemptions.
    expect(within(dialog).queryByText('api.example.com')).toBeNull()

    await user.clear(within(dialog).getByRole('textbox', { name: '规则名称' }))
    await user.type(within(dialog).getByRole('textbox', { name: '规则名称' }), '路径穿越')
    await user.click(within(dialog).getByText(/适用范围/u))
    const methods = within(dialog).getByRole('textbox', { name: /限定 Method/u })
    const cidrs = within(dialog).getByRole('textbox', { name: /限定来源 CIDR/u })
    await user.type(methods, 'get, POST')
    await user.type(cidrs, '10.0.0.0/8， 192.168.0.0/16')
    expect((methods as HTMLInputElement).value).toBe('get, POST')

    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())

    // Applying only changes the working copy; the row says so until saved.
    expect(screen.getByText('路径穿越')).toBeTruthy()
    expect(screen.getByText('已修改')).toBeTruthy()
    expect(screen.getByText(/未保存规则：新增 0 · 修改 1 · 移除 0/u)).toBeTruthy()
    expect(savedPolicyBody(fetchMock)).toBeUndefined()

    await user.click(screen.getByRole('button', { name: '保存策略草稿' }))
    await waitFor(() => {
      const rule = savedPolicyBody(fetchMock)?.rules[0]
      expect(rule?.name).toBe('路径穿越')
      expect(rule?.scope).toEqual({
        methods: ['GET', 'POST'],
        sourceCidrs: ['10.0.0.0/8', '192.168.0.0/16'],
      })
    })
    await waitFor(() => expect(screen.queryByText('已修改')).toBeNull())
  })

  test('adds a rule only once it is complete and checks it in isolation', async () => {
    const fetchMock = installApiMock()
    const user = await openPolicyPage()

    await user.click(screen.getByRole('button', { name: '向 核心规则 添加规则' }))
    const dialog = await screen.findByRole('dialog', { name: '添加规则' })
    expect(within(dialog).queryByRole('button', { name: '删除规则' })).toBeNull()

    // Nothing reaches the draft while required fields are missing.
    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    expect(await within(dialog).findByText('请填写规则名称')).toBeTruthy()
    expect(within(dialog).getByText('请填写 match 表达式')).toBeTruthy()
    expect(screen.getByText(/规则（1\/256）/u)).toBeTruthy()

    await user.type(within(dialog).getByRole('textbox', { name: '规则名称' }), '封禁扫描器')
    await user.clear(within(dialog).getByRole('textbox', { name: '优先级' }))
    await user.type(within(dialog).getByRole('textbox', { name: '优先级' }), '5')
    await user.type(
      within(dialog).getByRole('textbox', { name: 'match 表达式' }),
      '$req.path == "/scan"',
    )
    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())

    expect(screen.getByText(/规则（2\/256）/u)).toBeTruthy()
    expect(screen.getByText('新增')).toBeTruthy()

    // Priority 5 runs first, so it is listed first.
    const rows = screen.getAllByRole('listitem').map((row) => row.textContent ?? '')
    expect(rows.findIndex((text) => text.includes('封禁扫描器'))).toBeLessThan(
      rows.findIndex((text) => text.includes('路径穿越探测')),
    )

    // The check compiled the rule alone, in a mode that always yields a script.
    const checked = previewBodies(fetchMock).find((body) => body.rules[0]?.name === '封禁扫描器')
    expect(checked?.rules.length).toBe(1)
    expect(checked?.groups).toMatchObject([{ id: 'core', mode: 'log' }])
  })

  test('blocks applying a rule the validator rejects and points at the rule field', async () => {
    const fetchMock = installApiMock()
    const respondWithPreview = fetchMock.getMockImplementation()!
    fetchMock.mockImplementation(async (url, init) => {
      const body =
        url === '/api/waf/policy/compile-preview'
          ? (JSON.parse(String(init?.body)) as WafPolicy)
          : null
      if (body?.rules[0]?.match.includes('nope')) {
        return jsonResponse({
          ...compilePreview,
          scriptValid: false,
          scriptErrors: [
            {
              code: 'script_compile_failed',
              functionName: 'waf-core',
              field: 'rules[0].match',
              offset: 321,
              line: 1,
              column: 9,
              message: 'function not defined',
            },
          ],
        })
      }
      if (body?.rules[0]?.scope?.sourceCidrs?.includes('10.0.0.0/33')) {
        return jsonResponse({
          ...compilePreview,
          valid: false,
          compilation: null,
          scriptValid: null,
          issues: [
            {
              field: 'rules[0].scope.sourceCidrs',
              code: 'invalid_field',
              message: "invalid CIDR literal '10.0.0.0/33'",
            },
          ],
        })
      }
      return respondWithPreview(url, init)
    })
    const user = await openPolicyPage()

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    const dialog = await screen.findByRole('dialog', { name: '编辑规则' })
    const match = within(dialog).getByRole('textbox', { name: 'match 表达式' })
    await user.clear(match)
    await user.type(match, 'strings.nope($req.path)')
    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))

    expect(await within(dialog).findByText(/第 1 行第 9 列：function not defined/u)).toBeTruthy()
    expect(screen.getByRole('dialog', { name: '编辑规则' })).toBeTruthy()

    // A field issue inside the collapsed scope section opens it.
    await user.clear(match)
    await user.type(match, '$req.path == "/x"')
    await user.click(within(dialog).getByText(/适用范围/u))
    await user.type(within(dialog).getByRole('textbox', { name: /限定来源 CIDR/u }), '10.0.0.0/33')
    await user.click(within(dialog).getByText(/适用范围/u))
    expect(await within(dialog).findByText(/invalid CIDR literal/u)).toBeTruthy()
    await waitFor(() => {
      expect(
        within(dialog)
          .getByText(/适用范围/u)
          .closest('details')?.open,
      ).toBe(true)
    })
    expect(
      within(dialog)
        .getByRole('textbox', { name: /限定来源 CIDR/u })
        .getAttribute('aria-invalid'),
    ).toBe('true')
  })

  test('still applies a rule while the validator is unreachable', async () => {
    const fetchMock = installApiMock()
    const respondWithPreview = fetchMock.getMockImplementation()!
    fetchMock.mockImplementation(async (url, init) =>
      url === '/api/waf/policy/compile-preview' &&
      (JSON.parse(String(init?.body)) as typeof policy).rules[0]?.name === '断网时编辑'
        ? jsonResponse({ error: { message: 'Native Validator unavailable' } }, 503)
        : respondWithPreview(url, init),
    )
    const user = await openPolicyPage()

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    const dialog = await screen.findByRole('dialog', { name: '编辑规则' })
    await user.clear(within(dialog).getByRole('textbox', { name: '规则名称' }))
    await user.type(within(dialog).getByRole('textbox', { name: '规则名称' }), '断网时编辑')
    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))

    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(screen.getByText('断网时编辑')).toBeTruthy()
  })

  test('asks before dropping unapplied edits and removes rules from the dialog', async () => {
    installApiMock()
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(false)
    const user = await openPolicyPage()

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    let dialog = await screen.findByRole('dialog', { name: '编辑规则' })

    // Untouched: Escape closes straight away and focus returns to the row.
    await user.keyboard('{Escape}')
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(confirm).not.toHaveBeenCalled()
    expect(document.activeElement).toBe(
      screen.getByRole('button', { name: '编辑规则 路径穿越探测' }),
    )

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    dialog = await screen.findByRole('dialog', { name: '编辑规则' })
    await user.type(within(dialog).getByRole('textbox', { name: '规则名称' }), '改')
    await user.keyboard('{Escape}')
    expect(confirm).toHaveBeenCalledOnce()
    expect(screen.getByRole('dialog', { name: '编辑规则' })).toBeTruthy()

    confirm.mockReturnValue(true)
    await user.click(within(dialog).getByRole('button', { name: '取消' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(screen.getByText('路径穿越探测')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    dialog = await screen.findByRole('dialog', { name: '编辑规则' })
    await user.click(within(dialog).getByRole('button', { name: '删除规则' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(screen.queryByText('路径穿越探测')).toBeNull()
    expect(screen.getByText(/规则（0\/256）/u)).toBeTruthy()
  })

  test('edits a group scope and exemptions in its dialog', async () => {
    const fetchMock = installApiMock()
    const user = await openPolicyPage()
    expect(screen.getByText('waf-core · 优先级 100 · 全部 Project · 豁免 1')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '编辑规则组 核心规则' }))
    const dialog = await screen.findByRole('dialog', { name: '编辑规则组' })
    // The id is the wire identity: a group can never be renamed in place.
    expect(
      (within(dialog).getByRole('textbox', { name: '组 ID' }) as HTMLInputElement).disabled,
    ).toBe(true)

    await user.click(within(dialog).getByRole('button', { name: '移除豁免' }))
    expect(within(dialog).getByText('豁免 Project（0）')).toBeTruthy()

    await user.type(within(dialog).getByRole('textbox', { name: '豁免 Project' }), 'loan-*')
    const add = within(dialog).getByRole('button', { name: '添加豁免' })
    expect((add as HTMLButtonElement).disabled).toBe(true)
    await user.type(within(dialog).getByRole('textbox', { name: '豁免原因' }), '贷款线单独治理')
    await user.click(add)
    expect(within(dialog).getByText('贷款线单独治理')).toBeTruthy()

    const include = within(dialog).getByRole('textbox', { name: /生效 Project/u })
    await user.clear(include)
    await user.type(include, 'loan-*, trade-web，api.example.com')

    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(screen.getByText('waf-core · 优先级 100 · 3 条 Project 匹配 · 豁免 1')).toBeTruthy()
    expect(screen.getByText('已修改')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '保存策略草稿' }))
    await waitFor(() => {
      expect(savedPolicyBody(fetchMock)?.groups[0]?.projects).toEqual({
        include: ['loan-*', 'trade-web', 'api.example.com'],
        exempt: [{ project: 'loan-*', reason: '贷款线单独治理' }],
      })
    })
  })

  test('creates a group, moves a rule into it, and deletes a group with its rules', async () => {
    installApiMock()
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(false)
    const user = await openPolicyPage()

    await user.click(screen.getByRole('button', { name: '+ 新建规则组' }))
    let dialog = await screen.findByRole('dialog', { name: '新建规则组' })

    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    expect(await within(dialog).findByText('请填写组名称')).toBeTruthy()
    await user.type(within(dialog).getByRole('textbox', { name: '组 ID' }), 'core')
    expect(await within(dialog).findByText('组 ID core 已存在')).toBeTruthy()

    await user.type(within(dialog).getByRole('textbox', { name: '组名称' }), '灰度规则')
    await user.clear(within(dialog).getByRole('textbox', { name: '组 ID' }))
    await user.type(within(dialog).getByRole('textbox', { name: '组 ID' }), 'canary')
    expect(within(dialog).getByText('waf-canary')).toBeTruthy()
    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())

    // A new group starts off so creating it changes nothing on the wire.
    const created = screen.getByRole('region', { name: '规则组 灰度规则' })
    expect(within(created).getByText('关闭')).toBeTruthy()
    expect(within(created).getByText(/本组还没有规则/u)).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    dialog = await screen.findByRole('dialog', { name: '编辑规则' })
    await user.selectOptions(within(dialog).getByRole('combobox', { name: '规则组' }), 'canary')
    await user.click(within(dialog).getByRole('button', { name: '应用到草稿' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(
      within(screen.getByRole('region', { name: '规则组 灰度规则' })).getByText('路径穿越探测'),
    ).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '编辑规则组 灰度规则' }))
    dialog = await screen.findByRole('dialog', { name: '编辑规则组' })
    await user.click(within(dialog).getByRole('button', { name: '删除规则组' }))
    expect(confirm).toHaveBeenCalledWith('删除该规则组及其 1 条规则吗？')
    expect(screen.getByRole('dialog', { name: '编辑规则组' })).toBeTruthy()

    confirm.mockReturnValue(true)
    await user.click(within(dialog).getByRole('button', { name: '删除规则组' }))
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(screen.queryByRole('region', { name: '规则组 灰度规则' })).toBeNull()
    expect(screen.queryByText('路径穿越探测')).toBeNull()
  })

  test('saving from the editor shortcut applies the rule', async () => {
    installApiMock()
    const user = await openPolicyPage()

    await user.click(screen.getByRole('button', { name: '编辑规则 路径穿越探测' }))
    const dialog = await screen.findByRole('dialog', { name: '编辑规则' })
    const match = within(dialog).getByRole('textbox', { name: 'match 表达式' })
    await user.type(match, ' && true')
    await user.keyboard('{Control>}s{/Control}')

    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    expect(screen.getByText('已修改')).toBeTruthy()
  })

  test('shows field issues from the whole-policy preview on the rule row', async () => {
    const fetchMock = installApiMock()
    const respondWithPreview = fetchMock.getMockImplementation()!
    fetchMock.mockImplementation(async (url, init) =>
      url === '/api/waf/policy/compile-preview'
        ? jsonResponse({
            ...compilePreview,
            valid: false,
            compilation: null,
            scriptValid: null,
            issues: [
              {
                field: 'rules[0].scope.paths',
                code: 'invalid_field',
                message: "invalid path pattern 'admin'",
              },
            ],
          })
        : respondWithPreview(url, init),
    )
    await openPolicyPage(false)

    expect(await screen.findByText('1 个问题', { selector: '.waf-rule-row *' })).toBeTruthy()
    expect(screen.getByText('修复下列问题后才能生成脚本。')).toBeTruthy()
    expect(screen.getByText('路径穿越探测 · scope.paths')).toBeTruthy()
  })

  test('publishes and rolls back WAF releases with preview diff context', async () => {
    const fetchMock = installApiMock()
    vi.spyOn(window, 'confirm').mockReturnValue(true)
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf/releases'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByText('R5')).toBeTruthy()
    expect(screen.getByText(/挂载 1 个 WAF 函数 · 1 条规则/u)).toBeTruthy()
    expect(screen.getByText('函数脚本')).toBeTruthy()
    expect(screen.getByText('函数索引')).toBeTruthy()
    // The preview compares the planned index (WAF + handwritten drafts)
    // per-function against the live index in rnacos.
    expect(await screen.findByText(/索引将发布 2 个函数（WAF 生成 \+ 手写合并）/u)).toBeTruthy()
    expect(screen.getByText('变更（脚本）')).toBeTruthy()
    expect(screen.getByText('新增')).toBeTruthy()
    expect(screen.getByText('残留')).toBeTruthy()
    // The preview diffs the compiled draft script against the live script.
    expect(await screen.findByText(/与 rnacos 当前脚本的差异（\+1 \/ -1 行）/u)).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '发布到 rnacos' }))
    await waitFor(() => {
      const publishCall = fetchMock.mock.calls.find(
        ([url, init]) =>
          url === `/api/waf/releases/${releaseId}/publication` && init?.method === 'POST',
      )
      expect(publishCall).toBeTruthy()
      expect(new Headers(publishCall?.[1]?.headers).get('Idempotency-Key')).toMatch(/^.+$/u)
    })

    await user.click(screen.getByRole('button', { name: '回滚到此版本' }))
    await waitFor(() => {
      const rollbackCall = fetchMock.mock.calls.find(
        ([url, init]) =>
          url === `/api/waf/releases/${releaseId}/rollback` && init?.method === 'POST',
      )
      expect(rollbackCall).toBeTruthy()
    })
  })

  test('authors handwritten edge functions with per-script compile preview and If-Match save', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf/edge-functions'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: '边缘函数' })).toBeTruthy()
    // The draft workspace lists the saved function and opens its editor.
    expect(await screen.findByText(/1 个启用函数待发布/u)).toBeTruthy()
    const scriptEditor = (await screen.findByRole('textbox', {
      name: '函数 bot-trap 脚本',
    })) as HTMLTextAreaElement
    expect(scriptEditor.value).toContain('ctx.report')
    expect(screen.getByText(/抓扫描器/u)).toBeTruthy()
    expect(screen.getByText(/脚本可编译/u)).toBeTruthy()

    // Editing the script marks the draft dirty; the debounced compile preview
    // posts the working draft.
    await user.clear(scriptEditor)
    await user.type(scriptEditor, 'return false;')
    await waitFor(() => {
      const calls = fetchMock.mock.calls.filter(
        ([url, init]) =>
          url === '/api/waf/edge-functions/compile-preview' && init?.method === 'POST',
      )
      const postedEditedDraft = calls.some((call) => {
        const body = JSON.parse(String(call?.[1]?.body)) as { functions: { script: string }[] }
        return body.functions[0]?.script === 'return false;'
      })
      expect(postedEditedDraft).toBe(true)
    })

    await user.click(screen.getByRole('button', { name: '保存函数草稿' }))
    await waitFor(() => {
      const saveCall = fetchMock.mock.calls.find(
        ([url, init]) => url === '/api/waf/edge-functions/draft' && init?.method === 'PUT',
      )
      expect(saveCall).toBeTruthy()
      expect(new Headers(saveCall?.[1]?.headers).get('If-Match')).toBe('"2"')
      const body = JSON.parse(String(saveCall?.[1]?.body)) as {
        draft: typeof edgeFunctionsDraft.draft
      }
      expect(body.draft.functions[0]?.script).toBe('return false;')
      expect(body.draft.functions[0]?.ratio).toBe(5000)
    })

    // Switching the function to the request_body phase annotates the editor,
    // rides the compile preview, and reaches the saved draft.
    const phaseSelect = screen.getByRole('combobox', { name: '函数 bot-trap 执行阶段' })
    await user.selectOptions(phaseSelect, 'request_body')
    expect(screen.getByText(/\$req\.body 仅在该阶段可引用/u)).toBeTruthy()
    await waitFor(() => {
      const calls = fetchMock.mock.calls.filter(
        ([url, init]) =>
          url === '/api/waf/edge-functions/compile-preview' && init?.method === 'POST',
      )
      const phased = calls.some((call) => {
        const body = JSON.parse(String(call?.[1]?.body)) as {
          functions: { phase?: string; script: string }[]
        }
        return body.functions[0]?.phase === 'request_body'
      })
      expect(phased).toBe(true)
    })

    // The live rnacos snapshot stays visible below the authoring surface.
    expect(screen.getByText('索引版本：5')).toBeTruthy()
    expect(screen.getByText(/exclude=api\.example\.com,legacy\.example\.com/u)).toBeTruthy()
    expect(screen.getByText(/old-fn/u)).toBeTruthy()
    expect(screen.getByText(/请求体阶段/u)).toBeTruthy()
  })

  test('starts a version canary and only promotes it once removing the target is a no-op', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf/edge-functions'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await user.click(await screen.findByRole('button', { name: '创建灰度版本' }))

    // The new version opens as a 1% canary of its source, same script and scope.
    const ratio = (await screen.findByRole('spinbutton', {
      name: '函数 bot-trap-v2 采样比例',
    })) as HTMLInputElement
    expect(ratio.value).toBe('100')
    const replaces = screen.getByRole('combobox', {
      name: '函数 bot-trap-v2 取代函数',
    }) as HTMLSelectElement
    expect(replaces.value).toBe('bot-trap')
    expect(screen.getByText(/这不是异常兜底/u)).toBeTruthy()
    expect(
      (screen.getByRole('button', { name: '创建灰度版本' }) as HTMLButtonElement).disabled,
    ).toBe(true)

    // Below full ratio the target still serves traffic, so promotion is blocked.
    const promote = screen.getByRole('button', { name: '转正' }) as HTMLButtonElement
    expect(promote.disabled).toBe(true)
    expect(promote.title).toContain('10000')

    // At full ratio a narrower scope would unmount the target elsewhere.
    fireEvent.change(ratio, { target: { value: '10000' } })
    const include = screen.getByRole('textbox', {
      name: '函数 bot-trap-v2 include',
    }) as HTMLInputElement
    fireEvent.change(include, { target: { value: 'loan-web' } })
    expect(promote.disabled).toBe(true)
    expect(promote.title).toContain('挂载范围')

    fireEvent.change(include, { target: { value: 'loan-*' } })
    expect(promote.disabled).toBe(false)
    await user.click(promote)

    // The target is gone and the survivor is an ordinary full-traffic function.
    expect(screen.queryByRole('button', { name: '转正' })).toBeNull()
    await user.click(screen.getByRole('button', { name: '保存函数草稿' }))
    await waitFor(() => {
      const saveCall = fetchMock.mock.calls.find(
        ([url, init]) => url === '/api/waf/edge-functions/draft' && init?.method === 'PUT',
      )
      expect(saveCall).toBeTruthy()
      const body = JSON.parse(String(saveCall?.[1]?.body)) as {
        draft: { functions: { name: string; ratio?: number; replaces?: string }[] }
      }
      expect(body.draft.functions.map((fn) => fn.name)).toEqual(['bot-trap-v2'])
      expect(body.draft.functions[0]?.ratio).toBeUndefined()
      expect(body.draft.functions[0]?.replaces).toBeUndefined()
    })
  })

  test('shows WAF hit statistics grouped per rule with joined policy metadata', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf/hits'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: '命中统计' })).toBeTruthy()
    // Rows are grouped by rule and joined with the draft policy (probe-0001 → 路径穿越探测);
    // unknown ids stay visible as historical rules.
    expect(await screen.findByText('路径穿越探测')).toBeTruthy()
    expect(screen.getByText('核心规则')).toBeTruthy()
    expect(screen.getByText(/共 2 条规则 · 拦截 11 次 · 记录 4 次/u)).toBeTruthy()
    expect(screen.getByText(/retired-rule · 历史规则或外部函数事件/u)).toBeTruthy()
    expect(screen.getByText(/已不在当前策略/u)).toBeTruthy()

    // Switching the window refetches with the new query parameter.
    await user.click(screen.getByRole('radio', { name: /近 7 天/u }))
    expect(await screen.findByText(/规则命中（近 7 天/u)).toBeTruthy()
  })

  test('manages per-project mode override and exemptions from the project page', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/waf-policy`],
    })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByText('跟随各规则组模式')).toBeTruthy()
    // This project is exempt from one group and outside the other.
    expect(screen.getByText('误报待修')).toBeTruthy()
    expect(screen.getByText('未包含该 Project')).toBeTruthy()

    await user.click(screen.getByRole('radio', { name: /^仅记录/u }))
    expect(await screen.findByText('模式覆盖：仅记录')).toBeTruthy()

    // Once the exemption is gone the group applies, under the override.
    await user.click(screen.getByRole('button', { name: '移除豁免' }))
    expect(await screen.findByText('模式：仅记录')).toBeTruthy()
    expect(screen.queryByText('误报待修')).toBeNull()

    // Only groups that apply can be exempted from.
    const groupSelect = screen.getByRole('combobox', { name: '选择要豁免的规则组' })
    expect(within(groupSelect).queryByRole('option', { name: '贷款规则' })).toBeNull()
    await user.selectOptions(groupSelect, 'core')
    await user.type(screen.getByRole('textbox', { name: '豁免原因' }), '接口回放误报')
    await user.click(screen.getByRole('button', { name: '添加豁免' }))
    expect(await screen.findByText('接口回放误报')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '保存到环境草稿' }))

    await waitFor(() => {
      const saveCall = fetchMock.mock.calls.find(
        ([url, init]) => url === '/api/waf/policy' && init?.method === 'PUT',
      )
      expect(saveCall).toBeTruthy()
      expect(new Headers(saveCall?.[1]?.headers).get('If-Match')).toBe('"3"')
      const body = JSON.parse(String(saveCall?.[1]?.body)) as { policy: typeof policy }
      expect(body.policy.projectModes).toEqual({
        'legacy.example.com': 'off',
        'api.example.com': 'log',
      })
      expect(body.policy.groups[0]?.projects).toEqual({
        include: ['*'],
        exempt: [{ project: 'api.example.com', reason: '接口回放误报' }],
      })
      expect(body.policy.groups[1]?.projects).toEqual({ include: ['loan-*'] })
    })
  })

  test('never reports a group as in effect while it cannot run on the project', async () => {
    const fetchMock = installApiMock()
    const respond = fetchMock.getMockImplementation()!
    const openWith = async (draftPolicy: unknown) => {
      fetchMock.mockImplementation(async (url, init) =>
        url === '/api/waf/policy' && !init?.method
          ? jsonResponse({ ...policyDraft, policy: draftPolicy })
          : respond(url, init),
      )
      render(
        <RouterProvider
          router={createMemoryRouter(appRoutes, {
            initialEntries: [`/projects/${projectId}/waf-policy`],
          })}
        />,
      )
    }

    const core = { ...policy.groups[0], projects: { include: ['*'] } }
    await openWith({ ...policy, groups: [{ ...core, mode: 'off' }] })
    expect(await screen.findByText('规则组已关闭')).toBeTruthy()
    expect(screen.getByText('未开启')).toBeTruthy()
    expect(screen.queryByText('生效')).toBeNull()
    cleanup()

    await openWith({
      ...policy,
      groups: [core],
      projectModes: { 'api.example.com': 'off' },
    })
    expect(await screen.findByText('该 Project 的模式覆盖为关闭')).toBeTruthy()
    expect(screen.queryByText('生效')).toBeNull()
  })

  test('points a project exempted by a glob at the WAF page', async () => {
    const fetchMock = installApiMock()
    const respond = fetchMock.getMockImplementation()!
    fetchMock.mockImplementation(async (url, init) =>
      url === '/api/waf/policy' && !init?.method
        ? jsonResponse({
            ...policyDraft,
            policy: {
              ...policy,
              groups: [
                {
                  ...policy.groups[0],
                  projects: {
                    include: ['*'],
                    exempt: [{ project: '*.example.com', reason: '站点统一豁免' }],
                  },
                },
              ],
            },
          })
        : respond(url, init),
    )
    render(
      <RouterProvider
        router={createMemoryRouter(appRoutes, {
          initialEntries: [`/projects/${projectId}/waf-policy`],
        })}
      />,
    )

    expect(
      await screen.findByText('由 *.example.com 豁免：站点统一豁免（请在 WAF 页修改）'),
    ).toBeTruthy()
    expect(screen.getByText('已豁免')).toBeTruthy()
    expect(screen.queryByRole('button', { name: '移除豁免' })).toBeNull()
  })

  test('reserves generated function names in the handwritten workspace', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf/edge-functions'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await user.type(await screen.findByRole('textbox', { name: '新函数名' }), 'waf-sqli')
    await user.click(screen.getByRole('button', { name: '+ 新增函数' }))
    expect(await screen.findByText(/waf 与 waf-\* 为生成函数保留/u)).toBeTruthy()
  })
})
