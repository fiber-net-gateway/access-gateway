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
        mounts: [{ projects: { include: ['*'], exclude: ['legacy.example.com'] }, priority: 100 }],
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

  test('shows the live edge-function index and script bytes read-only', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/waf/edge-functions'] })
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: '边缘函数（只读）' })).toBeTruthy()
    expect(screen.getByText('索引版本：5')).toBeTruthy()
    expect(screen.getByText('函数数量：1')).toBeTruthy()
    expect(screen.getByText('waf')).toBeTruthy()
    expect(screen.getByText(/exclude=legacy\.example\.com/u)).toBeTruthy()
    expect(screen.getByText('查看脚本内容')).toBeTruthy()
    expect(screen.getByText(/return 0/u)).toBeTruthy()
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
