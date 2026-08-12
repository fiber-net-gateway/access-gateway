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

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  })
}

function installApiMock() {
  const project = {
    id: projectId,
    domain: 'api.example.com',
    status: 'active',
    draft: { id: 'draft-id', state: 'editing', revision: 8, lockVersion: '8' },
    publishedVersion: null,
    activationStatus: 'unknown',
    certificate: null,
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
    schemaVersion: 3,
    kind: 'project_routes_yaml',
    networkPolicy: { source: 'route', allowedCidrs: [], deniedCidrs: [] },
    routes: [],
  }
  const historicalModel = {
    schemaVersion: 3,
    kind: 'project_routes_yaml',
    networkPolicy: { source: 'route', allowedCidrs: [], deniedCidrs: [] },
    routes: [{ id: routeId, source: 'path: /historical\ntype: RESPONSE\nstatus: 200' }],
  }
  const certificate = {
    id: '00000000-0000-4000-8000-000000000006',
    name: 'API certificate',
    status: 'valid',
    subject: 'CN=api.example.com',
    issuer: 'CN=Test CA',
    serialNumber: '01',
    fingerprintSha256: 'a'.repeat(64),
    dnsNames: ['api.example.com'],
    notBefore: '2026-01-01T00:00:00.000Z',
    notAfter: '2027-01-01T00:00:00.000Z',
    keyType: 'ec',
    bindingCount: 1,
    runtimeDeploymentStatus: 'unsupported',
    createdAt: '2026-01-01T00:00:00.000Z',
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
          },
          publicationWorker: { status: 'ready', detail: 'ready' },
          activationCollector: { status: 'unavailable', detail: 'not implemented' },
        },
      })
    }
    if (url === `/api/projects/${projectId}`) return jsonResponse(project)
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
    if (url === `/api/projects/${projectId}/certificate`) {
      return jsonResponse({
        projectId,
        domain: 'api.example.com',
        certificate,
        coverageStatus: 'covered',
        runtimeDeploymentStatus: 'unsupported',
        boundAt: '2026-08-12T00:00:00.000Z',
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

describe('application routes', () => {
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

  test('saves a Project-owned CIDR policy as a new immutable configuration version', async () => {
    const fetchMock = installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/network-policy`],
    })
    const user = userEvent.setup()
    render(<RouterProvider router={router} />)

    await screen.findByRole('heading', { name: 'Network Policy' })
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
            allowedCidrs: string[]
            deniedCidrs: string[]
          }
        }
      }
      expect(body.model.networkPolicy).toEqual({
        source: 'project',
        allowedCidrs: ['10.0.0.0/8'],
        deniedCidrs: ['10.1.0.0/16'],
      })
    })
  })

  test('shows certificate SAN coverage separately from unsupported runtime deployment', async () => {
    installApiMock()
    const router = createMemoryRouter(appRoutes, {
      initialEntries: [`/projects/${projectId}/certificate`],
    })
    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: 'Certificate' })).toBeTruthy()
    expect(await screen.findByText('SAN 已覆盖')).toBeTruthy()
    expect(screen.getAllByText(/运行时部署未接入/u).length).toBeGreaterThan(0)
    expect(screen.queryByText('已激活')).toBeNull()
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
