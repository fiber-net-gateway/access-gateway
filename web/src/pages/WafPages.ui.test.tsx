import { cleanup, render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, test, vi } from 'vitest'
import { createMemoryRouter } from 'react-router'
import { RouterProvider } from 'react-router/dom'

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
  schemaVersion: 1,
  defaultMode: 'log',
  rules: [
    {
      id: 'probe-0001',
      name: '路径穿越探测',
      severity: 'high',
      priority: 100,
      action: 'block',
      phase: 'request_head',
      match: 'strings.contains($req.path, "../")',
      exclusions: [{ project: 'api.example.com', reason: '误报待修' }],
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
    script: 'function waf() {\n  return 1\n}\n',
    scriptSha256: 'a'.repeat(64),
    functionName: 'waf',
    mountPriority: 100,
    mounts: [{ include: ['*'], exclude: ['legacy.example.com'] }],
    ruleOrder: ['probe-0001'],
    directives: [],
  },
  scriptValid: true,
  scriptErrors: [],
  validator: { contractVersion: 1, revision: 'test' },
}

const releaseView = {
  id: releaseId,
  sequence: '5',
  status: 'ready',
  functionName: 'waf',
  mode: 'mounted',
  scriptSha256: '1'.repeat(64),
  ruleCount: 1,
  indexVersion: '5',
  description: '',
  resources: [
    {
      id: '00000000-0000-4000-8000-000000000010',
      kind: 'edge_function_script',
      dataId: 'ploto.unified-access.edge-function.waf',
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
        name: 'waf',
        sha256: '1'.repeat(64),
        phase: null,
        mounts: [
          {
            projects: { include: ['*'], exclude: ['legacy.example.com'] },
            priority: 100,
            ratio: null,
          },
        ],
      },
      {
        name: 'old-fn',
        sha256: '9'.repeat(64),
        phase: 'request_body',
        mounts: [{ projects: { include: ['*'], exclude: [] }, priority: 500, ratio: null }],
      },
    ],
    parseError: null,
  },
  scripts: [
    {
      name: 'waf',
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
        mounts: [
          {
            projects: { include: ['loan-*'], exclude: ['loan-admin-legacy'] },
            priority: 200,
            ratio: 5000,
          },
        ],
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
  version: 'waf-rule-pack-v1',
  rules: [
    policy.rules[0], // same id as the draft rule → skipped on import
    {
      id: 'sqli-union-select',
      name: 'union select 注入探测',
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

describe('WAF console pages', () => {
  test('edits the policy draft and saves it with optimistic concurrency', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: 'WAF 策略' })).toBeTruthy()
    expect(screen.getByText('路径穿越探测')).toBeTruthy()
    const matchEditor = (await screen.findByRole('textbox', {
      name: '规则 1 match 表达式',
    })) as HTMLTextAreaElement
    expect(matchEditor.value).toContain('strings.contains')

    // The debounced compile preview renders the mounted compilation summary.
    expect(await screen.findByText(/挂载函数 waf · 脚本 \d+ 行/u)).toBeTruthy()

    // Importing the starter rule pack skips the existing id and appends the rest
    // into the dirty working copy.
    await user.click(screen.getByRole('button', { name: '导入起手规则包' }))
    expect(
      await screen.findByText(/已导入 1 条（waf-rule-pack-v1），跳过已存在 1 条/u),
    ).toBeTruthy()
    expect(await screen.findByText('union select 注入探测')).toBeTruthy()

    await user.click(screen.getByRole('radio', { name: /关闭/u }))
    await user.click(screen.getByRole('button', { name: '保存策略草稿' }))

    await waitFor(() => {
      const saveCall = fetchMock.mock.calls.find(
        ([url, init]) => url === '/api/waf/policy' && init?.method === 'PUT',
      )
      expect(saveCall).toBeTruthy()
      expect(new Headers(saveCall?.[1]?.headers).get('If-Match')).toBe('"3"')
      const body = JSON.parse(String(saveCall?.[1]?.body)) as { policy: typeof policy }
      expect(body.policy.defaultMode).toBe('off')
      expect(body.policy.rules.length).toBe(2)
      expect(body.policy.rules[1]?.id).toBe('sqli-union-select')
      expect(body.policy.rules[0]?.exclusions).toEqual([
        { project: 'api.example.com', reason: '误报待修' },
      ])
    })
  })

  test('publishes and rolls back WAF releases with preview diff context', async () => {
    const fetchMock = installApiMock()
    vi.spyOn(window, 'confirm').mockReturnValue(true)
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf/releases'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByText('R5')).toBeTruthy()
    expect(screen.getByText(/挂载函数 waf · 1 条规则/u)).toBeTruthy()
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
      expect(body.draft.functions[0]?.mounts[0]?.ratio).toBe(5000)
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
    expect(screen.getByText(/exclude=legacy\.example\.com/u)).toBeTruthy()
    expect(screen.getByText(/old-fn/u)).toBeTruthy()
    expect(screen.getByText(/请求体阶段/u)).toBeTruthy()
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

    expect(await screen.findByText('生效模式：仅记录（跟随环境默认）')).toBeTruthy()
    // The environment rule excludes this project with an auditable reason.
    expect(screen.getByText('误报待修')).toBeTruthy()

    await user.click(screen.getByRole('radio', { name: /^关闭/u }))
    expect(await screen.findByText('生效模式：关闭（本项目覆盖）')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '移除豁免' }))
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
        'api.example.com': 'off',
      })
      expect(body.policy.rules[0]?.exclusions).toEqual([])
    })
  })
})
