import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, test, vi } from 'vitest'
import { createMemoryRouter, Link } from 'react-router'
import { RouterProvider } from 'react-router/dom'

import { appRoutes } from './router'
import { useUnsavedChangesGuard } from './routes/useUnsavedChangesGuard'

vi.mock('./components/YamlCodeEditor', () => ({
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
const versionId = '00000000-0000-4000-8000-000000000002'
const historicalVersionId = '00000000-0000-4000-8000-000000000003'
const routeId = '00000000-0000-4000-8000-000000000004'
const certificateId = '00000000-0000-4000-8000-000000000006'

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  })
}

function installApiMock(options: { releaseEvidence?: boolean } = {}) {
  const project = {
    id: projectId,
    domain: 'api.example.com',
    status: 'active',
    lockVersion: '4',
    draft: { id: 'draft-id', state: 'editing', revision: 8, lockVersion: '8' },
    publishedVersion: null,
    activationStatus: 'unknown',
    createdAt: '2026-08-01T00:00:00.000Z',
    updatedAt: '2026-08-12T00:00:00.000Z',
  }
  const version = {
    id: versionId,
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
  }
  const historicalVersion = {
    ...version,
    id: historicalVersionId,
    number: 7,
    relation: 'historical',
    changeSummary: 'Historical routes',
    routeCount: 1,
  }
  const model = {
    schemaVersion: 5,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
    routes: [],
  }
  const historicalModel = {
    schemaVersion: 5,
    kind: 'project_routes_yaml',
    networkPolicy: {
      source: 'route',
      httpsRedirect: 'off',
      allowedCidrs: [],
      deniedCidrs: [],
    },
    routes: [
      {
        id: routeId,
        format: 'yaml',
        source: 'path: /historical\ntype: RESPONSE\nstatus: 200',
      },
    ],
  }
  const certificate = {
    id: certificateId,
    name: 'API certificate',
    lockVersion: '1',
    currentVersion: {
      id: '00000000-0000-4000-8000-000000000007',
      version: 2,
      status: 'valid',
      subject: 'CN=api.example.com',
      issuer: 'CN=Test CA',
      serialNumber: '01',
      fingerprintSha256: 'a'.repeat(64),
      dnsNames: ['api.example.com'],
      notBefore: '2026-01-01T00:00:00.000Z',
      notAfter: '2027-01-01T00:00:00.000Z',
      keyType: 'ec',
      createdAt: '2026-08-12T00:00:00.000Z',
    },
    versionCount: 2,
    runtimeDeploymentStatus: 'activation_unknown',
    createdAt: '2026-01-01T00:00:00.000Z',
    updatedAt: '2026-08-12T00:00:00.000Z',
  }
  const sniCertificate = {
    id: certificate.id,
    name: certificate.name,
    version: certificate.currentVersion.version,
    status: certificate.currentVersion.status,
    notAfter: certificate.currentVersion.notAfter,
    fingerprintSha256: certificate.currentVersion.fingerprintSha256,
    runtimeDeploymentStatus: 'activation_unknown',
  }
  const decommissionRelease = {
    id: '00000000-0000-4000-8000-000000000009',
    sequence: '9',
    projectId,
    kind: 'project_decommission',
    title: '下线 api.example.com',
    description: '域名已迁移',
    status: 'ready',
    sourceConfigurationVersion: null,
    currentConfigurationVersionAtCreation: { id: versionId, number: 8 },
    allocatedWireVersion: null,
    resources: [
      {
        id: '00000000-0000-4000-8000-000000000010',
        kind: 'project_list',
        dataId: 'ploto.unified-access.projects',
        group: 'ACCESS-SERVER',
        operation: 'upsert',
        status: 'pending',
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
    createdAt: '2026-08-13T00:00:00.000Z',
    publishedAt: null,
  }
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
          activationCollector: { status: 'unavailable', detail: 'not implemented' },
        },
      })
    }
    if (url === `/api/projects/${projectId}`) return jsonResponse(project)
    if (url === `/api/projects/${projectId}/releases`) {
      return jsonResponse({
        items: options.releaseEvidence
          ? [
              {
                ...decommissionRelease,
                status: 'published',
                publishedAt: '2026-08-13T00:01:00.000Z',
                activationStatus: 'active',
                activation: {
                  status: 'active',
                  targetCount: 1,
                  activeCount: 1,
                  pendingCount: 0,
                  degradedCount: 0,
                  unknownCount: 0,
                  evaluatedAt: '2026-08-13T00:01:05.000Z',
                },
              },
            ]
          : [],
      })
    }
    if (
      url === `/api/releases/${decommissionRelease.id}/activation?limit=50` &&
      options.releaseEvidence
    ) {
      return jsonResponse({
        releaseId: decommissionRelease.id,
        summary: {
          status: 'active',
          targetCount: 1,
          activeCount: 1,
          pendingCount: 0,
          degradedCount: 0,
          unknownCount: 0,
          evaluatedAt: '2026-08-13T00:01:05.000Z',
        },
        items: [
          {
            id: '00000000-0000-4000-8000-000000000012',
            instanceKey: 'access-0',
            status: 'active',
            buildVersion: '0.1.0',
            buildRevision: 'test-revision',
            evidenceRevision: '7',
            routeSnapshotGeneration: '3',
            routeSnapshotFingerprintSha256: 'a'.repeat(64),
            candidateStatus: 'accepted',
            candidateErrorCode: null,
            activeMd5: '1'.repeat(32),
            activeVersion: null,
            observedAt: '2026-08-13T00:01:05.000Z',
            expiresAt: '2026-08-13T00:01:20.000Z',
          },
        ],
        nextCursor: null,
      })
    }
    if (url === `/api/projects/${projectId}/decommission-releases` && init?.method === 'POST') {
      return jsonResponse(decommissionRelease, 201)
    }
    if (url === `/api/releases/${decommissionRelease.id}/publications` && init?.method === 'POST') {
      return jsonResponse(
        {
          jobId: '00000000-0000-4000-8000-000000000011',
          state: 'queued',
          release: {
            ...decommissionRelease,
            status: 'queued',
            publication: {
              jobId: '00000000-0000-4000-8000-000000000011',
              state: 'queued',
            },
          },
        },
        202,
      )
    }
    if (url === `/api/projects/${projectId}/configuration-versions/current`) {
      return jsonResponse({ version: { ...version, model }, lockVersion: '8' })
    }
    if (
      url === `/api/projects/${projectId}/configuration-versions` &&
      (!init?.method || init.method === 'GET')
    ) {
      return jsonResponse({
        items: [version, historicalVersion],
        nextCursor: null,
        currentVersionId: versionId,
        lockVersion: '8',
      })
    }
    if (url === `/api/projects/${projectId}/configuration-versions` && init?.method === 'POST') {
      const body = JSON.parse(String(init.body)) as { model: typeof model }
      return jsonResponse(
        {
          version: { ...version, number: 9, model: body.model },
          lockVersion: '9',
        },
        201,
      )
    }
    if (url === `/api/projects/${projectId}/configuration-versions/${historicalVersionId}`) {
      return jsonResponse({ ...historicalVersion, model: historicalModel })
    }
    if (
      url ===
        `/api/projects/${projectId}/configuration-versions/${historicalVersionId}/restorations` &&
      init?.method === 'POST'
    ) {
      const body = JSON.parse(String(init.body)) as { model: unknown }
      return jsonResponse(
        {
          version: {
            ...historicalVersion,
            id: '00000000-0000-4000-8000-000000000005',
            number: 9,
            relation: 'current',
            baseVersionId: versionId,
            restoredFromVersionId: historicalVersionId,
            model: body.model,
          },
          lockVersion: '9',
        },
        201,
      )
    }
    if (url === '/api/projects') return jsonResponse({ items: [project] })
    if (url === '/api/certificates') return jsonResponse({ items: [certificate] })
    if (url === '/api/tls/sni-resolution?serverName=api.example.com') {
      return jsonResponse({
        serverName: 'api.example.com',
        resolutionStatus: 'matched',
        matchKind: 'exact',
        certificate: sniCertificate,
        matches: [sniCertificate],
        runtimeDeploymentStatus: 'activation_unknown',
      })
    }
    if (url === `/api/certificates/${certificate.id}/versions` && init?.method === 'POST') {
      const body = JSON.parse(String(init.body)) as { confirmSniCoverageChange?: boolean }
      if (!body.confirmSniCoverageChange) {
        return jsonResponse(
          {
            error: {
              code: 'CERTIFICATE_SNI_COVERAGE_CONFIRMATION_REQUIRED',
              message: 'The certificate DNS SAN coverage changed',
              fields: [
                {
                  path: 'confirmSniCoverageChange',
                  code: 'SNI_NAME_ADDED',
                  message: 'new.example.com will start selecting this certificate',
                },
              ],
            },
          },
          409,
        )
      }
      return jsonResponse(
        {
          ...certificate,
          lockVersion: '2',
          currentVersion: { ...certificate.currentVersion, version: 3 },
          versionCount: 3,
        },
        201,
      )
    }
    if (url === `/api/certificates/${certificate.id}/versions`) {
      return jsonResponse({ items: [certificate.currentVersion] })
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

describe('application routes', () => {
  test('renders generated documentation and keeps the topic while switching language', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: ['/docs/en/script-reference'],
    })
    const user = userEvent.setup()

    render(<RouterProvider router={router} />)

    expect(
      await screen.findByRole('heading', {
        name: /Access Gateway script language, standard library, and HTTP API reference/u,
      }),
    ).toBeTruthy()
    expect(
      screen.getByRole('link', { name: /Script and API reference/u }).getAttribute('aria-current'),
    ).toBe('page')

    await user.click(screen.getByRole('link', { name: '简体中文' }))

    expect(
      await screen.findByRole('heading', {
        name: /Access Gateway 脚本语法、标准库与 HTTP API 参考/u,
      }),
    ).toBeTruthy()
    expect(router.state.location.pathname).toBe('/docs/zh-CN/script-reference')
  })

  test('opens a Project Routes page directly from its URL', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/routes`],
    })

    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: 'api.example.com' })).toBeTruthy()
    expect(await screen.findByRole('heading', { name: 'Routes' })).toBeTruthy()
    expect(screen.getByRole('link', { name: /Versions/u })).toBeTruthy()
    expect(router.state.location.pathname).toBe(`/projects/${projectId}/routes`)
  })

  test('creates a JavaScript route with external path and optional method fields', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/routes`],
    })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await screen.findByRole('heading', { name: 'Routes' })
    await user.click(screen.getByRole('button', { name: '+ JS' }))

    const path = screen.getByRole('textbox', { name: '路由 1 Path pattern' })
    const method = screen.getByRole('textbox', { name: '路由 1 Method' })
    const editor = await screen.findByRole('textbox', { name: /路由 1：SCRIPT/u })
    expect((path as HTMLInputElement).value).toBe('/script/:id')
    expect((method as HTMLInputElement).value).toBe('')
    expect((editor as HTMLTextAreaElement).value).toContain('$req.method')

    await user.clear(path)
    await user.type(path, '/jobs/:id')
    await user.type(method, 'POST')
    expect(await screen.findByText('POST · JS')).toBeTruthy()
    expect(screen.getByText('/jobs/:id')).toBeTruthy()
  })

  test('renders a stable not-found page for unknown Console URLs', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/missing'] })

    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: '页面不存在' })).toBeTruthy()
    expect(screen.getByRole('link', { name: '返回 Projects' })).toBeTruthy()
  })

  test('edits from a historical version and saves one derived version', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/versions`],
    })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await screen.findByRole('heading', { name: '配置版本' })
    await user.click(await screen.findByRole('button', { name: '基于此版本编辑' }))
    expect(await screen.findByRole('heading', { name: '以 V7 为起点编辑' })).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '进入 Routes 编辑' }))
    expect(await screen.findByText('正在基于历史 V7 编辑')).toBeTruthy()
    expect(router.state.location.search).toBe(`?sourceVersionId=${historicalVersionId}`)

    await user.click(screen.getByRole('button', { name: '保存为版本' }))
    expect(await screen.findByText('历史 V7')).toBeTruthy()
    await user.click(screen.getByRole('button', { name: '保存为 V9' }))

    await waitFor(() => {
      const restorationCall = fetchMock.mock.calls.find(
        ([url, init]) =>
          url ===
            `/api/projects/${projectId}/configuration-versions/${historicalVersionId}/restorations` &&
          init?.method === 'POST',
      )
      expect(restorationCall).toBeTruthy()
      const body = JSON.parse(String(restorationCall?.[1]?.body)) as {
        baseVersionId: string
        model: { routes: Array<{ source: string }> }
      }
      expect(body.baseVersionId).toBe(versionId)
      expect(body.model.routes[0]?.source).toContain('/historical')
    })
  })

  test('keeps editor focus while reporting invalid YAML and blocks every save entry', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/routes?sourceVersionId=${historicalVersionId}`],
    })
    render(<RouterProvider router={router} />)

    const editor = (await screen.findByRole('textbox', {
      name: /路由 1/u,
    })) as HTMLTextAreaElement
    editor.focus()
    fireEvent.change(editor, {
      target: {
        value: 'path: /\nstatus: 200\ntype: RESPONSE\nresponse_headers:\n  X-Heassf',
      },
    })

    expect(document.activeElement).toBe(editor)
    expect(await screen.findByText(/修复后才能保存/u)).toBeTruthy()
    expect((screen.getByRole('button', { name: '保存为版本' }) as HTMLButtonElement).disabled).toBe(
      true,
    )

    fireEvent.keyDown(editor, { key: 's', ctrlKey: true })

    expect(document.activeElement).toBe(editor)
    expect(await screen.findByText(/请修复后再保存为版本/u)).toBeTruthy()
    expect(screen.queryByRole('dialog', { name: '保存为配置版本' })).toBeNull()
  })

  test('saves HTTPS redirect and Project-owned CIDRs as one immutable version', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/network-policy`],
    })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await screen.findByRole('heading', { name: 'Network Policy' })
    await user.click(screen.getByRole('checkbox', { name: /强制 HTTPS/u }))
    await user.selectOptions(screen.getByRole('combobox', { name: /重定向状态码/u }), '307')
    await user.click(screen.getByRole('radio', { name: /Project 统一强制/u }))
    await user.type(screen.getByLabelText(/允许 CIDR/u), '10.0.0.0/8')
    await user.type(screen.getByLabelText(/拒绝 CIDR/u), '10.1.0.0/16')
    await user.click(screen.getByRole('button', { name: '保存为 V9' }))

    await waitFor(() => {
      const saveCall = fetchMock.mock.calls.find(
        ([url, init]) =>
          url === `/api/projects/${projectId}/configuration-versions` && init?.method === 'POST',
      )
      expect(saveCall).toBeTruthy()
      const body = JSON.parse(String(saveCall?.[1]?.body)) as {
        model: {
          networkPolicy: {
            source: string
            httpsRedirect: string
            allowedCidrs: string[]
            deniedCidrs: string[]
          }
        }
      }
      expect(body.model.networkPolicy).toEqual({
        source: 'project',
        httpsRedirect: '307',
        allowedCidrs: ['10.0.0.0/8'],
        deniedCidrs: ['10.1.0.0/16'],
      })
    })
  })

  test('creates and queues a Project decommission Release from Settings', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/settings`],
    })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: 'Settings' })).toBeTruthy()
    expect(screen.getAllByText('未知').length).toBeGreaterThan(0)
    await user.click(screen.getByRole('button', { name: '下线并归档 Project' }))
    expect(await screen.findByRole('heading', { name: '确认下线 api.example.com' })).toBeTruthy()

    await user.type(screen.getByLabelText('下线原因'), '域名已迁移')
    await user.type(screen.getByLabelText('输入完整域名以确认'), 'api.example.com')
    await user.click(screen.getByRole('button', { name: '创建并发布下线 Release' }))

    await waitFor(() => {
      const createCall = fetchMock.mock.calls.find(
        ([url, init]) =>
          url === `/api/projects/${projectId}/decommission-releases` && init?.method === 'POST',
      )
      expect(new Headers(createCall?.[1]?.headers).get('If-Match')).toBe('"4"')
      expect(JSON.parse(String(createCall?.[1]?.body))).toEqual({
        confirmationDomain: 'api.example.com',
        reason: '域名已迁移',
      })
    })
    await waitFor(() =>
      expect(router.state.location.pathname).toBe(`/projects/${projectId}/releases`),
    )
  })

  test('shows exact per-instance activation evidence on demand', async () => {
    installApiMock({ releaseEvidence: true })
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/releases`],
    })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByText('实例：已激活')).toBeTruthy()
    await user.click(screen.getByRole('button', { name: '查看实例证据' }))
    expect(await screen.findByText('access-0')).toBeTruthy()
    expect(screen.getByText('候选：accepted')).toBeTruthy()
    expect(screen.queryByText(/ACTIVATION_TOKEN/u)).toBeNull()
  })

  test('previews ClientHello SNI resolution independently from a Project', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/certificates'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: 'SNI 自动解析预览' })).toBeTruthy()
    await user.type(screen.getByLabelText('ClientHello server name'), 'api.example.com')
    await user.click(screen.getByRole('button', { name: '解析' }))
    expect(await screen.findByText('已匹配')).toBeTruthy()
    expect(screen.getByText('该预览基于当前库存；实际运行版本以最近发布快照为准。')).toBeTruthy()
    expect(screen.queryByText('已激活')).toBeNull()
  })

  test('updates one logical certificate through an immutable version workflow', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/certificates'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    expect(await screen.findByText('API certificate')).toBeTruthy()
    await user.click(screen.getByRole('button', { name: '更新证书版本' }))

    expect(await screen.findByRole('heading', { name: '更新 API certificate' })).toBeTruthy()
    expect(await screen.findByRole('heading', { name: '版本历史' })).toBeTruthy()
    expect(screen.getAllByText('V2').length).toBeGreaterThan(0)
    expect(screen.getByText(/DNS SAN 不变时直接续期/u)).toBeTruthy()
  })

  test('requires an explicit confirmation when a renewed certificate changes SNI coverage', async () => {
    const fetchMock = installApiMock()
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(true)
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/certificates'] })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await user.click(await screen.findByRole('button', { name: '更新证书版本' }))
    await user.type(screen.getByLabelText('PEM 证书链'), 'certificate material')
    await user.type(screen.getByLabelText('PEM 私钥'), 'private key material')
    await user.click(screen.getByRole('button', { name: '校验并更新当前版本' }))

    await waitFor(() => expect(confirm).toHaveBeenCalledTimes(1))
    await waitFor(() => {
      const updateCalls = fetchMock.mock.calls.filter(
        ([url, init]) =>
          url === `/api/certificates/${certificateId}/versions` && init?.method === 'POST',
      )
      expect(updateCalls).toHaveLength(2)
      const confirmedBody = JSON.parse(String(updateCalls[1]?.[1]?.body)) as {
        confirmSniCoverageChange?: boolean
      }
      expect(confirmedBody.confirmSniCoverageChange).toBe(true)
    })
  })
})

function DirtyRoute() {
  useUnsavedChangesGuard(true)
  return (
    <section>
      <h1>Dirty editor</h1>
      <Link to="/next">Leave editor</Link>
    </section>
  )
}

describe('unsaved Route navigation guard', () => {
  test('blocks an in-app navigation until the user confirms', async () => {
    const confirm = vi.spyOn(window, 'confirm').mockReturnValueOnce(false).mockReturnValueOnce(true)
    const router = createMemoryRouter(
      [
        { path: '/', element: <DirtyRoute /> },
        { path: '/next', element: <h1>Next page</h1> },
      ],
      { initialEntries: ['/'] },
    )
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await user.click(screen.getByRole('link', { name: 'Leave editor' }))
    await waitFor(() => expect(confirm).toHaveBeenCalledTimes(1))
    expect(router.state.location.pathname).toBe('/')

    await user.click(screen.getByRole('link', { name: 'Leave editor' }))
    await screen.findByRole('heading', { name: 'Next page' })
    expect(confirm).toHaveBeenCalledTimes(2)
    expect(router.state.location.pathname).toBe('/next')
  })

  test('marks browser unload events as canceled while the editor is dirty', () => {
    const router = createMemoryRouter([{ path: '/', element: <DirtyRoute /> }])
    render(<RouterProvider router={router} />)
    const event = new Event('beforeunload', { cancelable: true })

    window.dispatchEvent(event)

    expect(event.defaultPrevented).toBe(true)
  })
})
