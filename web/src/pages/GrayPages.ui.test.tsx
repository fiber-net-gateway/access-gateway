import { cleanup, fireEvent, render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, test, vi } from 'vitest'
import { createMemoryRouter } from 'react-router'
import { RouterProvider } from 'react-router/dom'

import type { GrayReleaseView } from '../api/types'
import { appRoutes } from '../router'

const releaseId = '50000000-0000-4000-8000-000000000090'
const rollbackReleaseId = '50000000-0000-4000-8000-000000000091'
const resourceId = '60000000-0000-4000-8000-000000000090'

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  })
}

function installGrayApiMock(options: { draftMissingOnce?: boolean } = {}) {
  let draftCreated = !options.draftMissingOnce
  let currentDraft = {
    id: '20000000-0000-4000-8000-000000000090',
    document: {
      schemaVersion: 1 as const,
      rules: [{ entry: 'internet' as const, ratio: 500, cidrs: ['10.0.0.0/8'] }],
    },
    documentSha256: 'a'.repeat(64),
    lockVersion: '1',
    createdAt: '2026-11-28T00:00:00.000Z',
    updatedAt: '2026-11-28T00:00:00.000Z',
  }
  let releases: GrayReleaseView[] = [
    {
      id: releaseId,
      sequence: '12',
      status: 'published',
      ruleCount: 1,
      entries: ['internet'],
      wireSha256: 'b'.repeat(64),
      description: 'initial canary',
      resource: {
        id: resourceId,
        dataId: 'ploto.unified-access.gray-match',
        group: 'DEFAULT_GROUP',
        status: 'verified',
        verifiedSha256: 'b'.repeat(64),
        verifiedAt: '2026-11-28T00:01:00.000Z',
      },
      publication: { jobId: '70000000-0000-4000-8000-000000000090', state: 'succeeded' },
      activationStatus: 'active',
      activation: {
        status: 'active',
        targetCount: 1,
        activeCount: 1,
        pendingCount: 0,
        degradedCount: 0,
        unknownCount: 0,
        evaluatedAt: '2026-11-28T00:01:05.000Z',
      },
      createdAt: '2026-11-28T00:00:00.000Z',
      publishedAt: '2026-11-28T00:01:00.000Z',
    },
  ]

  const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url
    if (url === '/api/health') {
      return jsonResponse({ status: 'ok', service: 'access-gateway-console-api', version: '0.1.0' })
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
            revision: 'native-rev',
            limits: null,
          },
          publicationWorker: { status: 'ready', detail: 'ready' },
          activationCollector: { status: 'ready', detail: 'ready' },
        },
      })
    }
    if (url === '/api/gray/draft' && (!init?.method || init.method === 'GET')) {
      if (!draftCreated) {
        return jsonResponse({ error: { code: 'NOT_FOUND', message: 'Not found' } }, 404)
      }
      return jsonResponse(currentDraft)
    }
    if (url === '/api/gray/draft' && init?.method === 'POST') {
      draftCreated = true
      currentDraft = {
        ...currentDraft,
        document: { schemaVersion: 1, rules: [] },
        lockVersion: '0',
      }
      return jsonResponse(currentDraft, 201)
    }
    if (url === '/api/gray/draft' && init?.method === 'PUT') {
      const body = JSON.parse(String(init.body)) as { document: typeof currentDraft.document }
      currentDraft = {
        ...currentDraft,
        document: body.document,
        lockVersion: String(Number(currentDraft.lockVersion) + 1),
      }
      return jsonResponse(currentDraft)
    }
    if (url === '/api/gray/validate' && init?.method === 'POST') {
      const doc = JSON.parse(String(init.body)) as {
        rules: { entry: string; ratio: number; cidrs: string[] }[]
      }
      const wire: Record<string, { ratio: number; cidrs: string[] }> = {}
      for (const rule of doc.rules) {
        wire[rule.entry] = { ratio: rule.ratio, cidrs: rule.cidrs }
      }
      return jsonResponse({
        valid: true,
        issues: [],
        wirePreview: JSON.stringify(wire),
        wireSha256: 'c'.repeat(64),
        dataId: 'ploto.unified-access.gray-match',
        group: 'DEFAULT_GROUP',
        ruleCount: doc.rules.length,
        nativeValid: true,
        nativeErrors: [],
        validator: { contractVersion: 1, revision: 'native-rev-123456' },
      })
    }
    if (url === '/api/gray/live') {
      return jsonResponse({
        exists: true,
        dataId: 'ploto.unified-access.gray-match',
        group: 'DEFAULT_GROUP',
        sha256: 'b'.repeat(64),
        md5: '1'.repeat(32),
        content: '{"internet":{"cidrs":["10.0.0.0/8"],"ratio":500}}',
        document: {
          schemaVersion: 1,
          rules: [{ entry: 'internet', ratio: 500, cidrs: ['10.0.0.0/8'] }],
        },
        parseError: null,
      })
    }
    if (url === '/api/gray/releases' && (!init?.method || init.method === 'GET')) {
      return jsonResponse({ items: releases })
    }
    if (url === '/api/gray/releases' && init?.method === 'POST') {
      const body = JSON.parse(String(init.body ?? '{}')) as { description?: string }
      const created = {
        ...releases[0]!,
        id: '50000000-0000-4000-8000-000000000092',
        sequence: '13',
        status: 'ready',
        description: body.description ?? '',
        resource: {
          ...releases[0]!.resource,
          status: 'pending',
          verifiedSha256: null,
          verifiedAt: null,
        },
        publication: { jobId: null, state: null },
        activationStatus: 'unknown' as const,
        activation: {
          status: 'unknown' as const,
          targetCount: 0,
          activeCount: 0,
          pendingCount: 0,
          degradedCount: 0,
          unknownCount: 0,
          evaluatedAt: null,
        },
        publishedAt: null,
      }
      releases = [created, ...releases]
      return jsonResponse(created, 201)
    }
    if (url === `/api/gray/releases/${releaseId}/rollback` && init?.method === 'POST') {
      const rolled = {
        ...releases[0]!,
        id: rollbackReleaseId,
        sequence: '14',
        status: 'ready',
        description: 'rollback of gray rules #12',
        resource: {
          ...releases[0]!.resource,
          status: 'pending',
          verifiedSha256: null,
          verifiedAt: null,
        },
        publication: { jobId: null, state: null },
        activationStatus: 'unknown' as const,
        activation: {
          status: 'unknown' as const,
          targetCount: 0,
          activeCount: 0,
          pendingCount: 0,
          degradedCount: 0,
          unknownCount: 0,
          evaluatedAt: null,
        },
        publishedAt: null,
      }
      releases = [rolled, ...releases]
      return jsonResponse(rolled, 201)
    }
    if (url === `/api/releases/${releaseId}/activation?limit=50`) {
      return jsonResponse({
        releaseId,
        summary: releases.find((item) => item.id === releaseId)!.activation,
        items: [
          {
            id: '80000000-0000-4000-8000-000000000090',
            instanceKey: 'access-gray-0',
            status: 'active',
            buildVersion: '0.1.0',
            buildRevision: 'a'.repeat(40),
            evidenceRevision: '4',
            routeSnapshotGeneration: '2',
            routeSnapshotFingerprintSha256: 'd'.repeat(64),
            candidateStatus: 'accepted',
            candidateErrorCode: null,
            activeMd5: '1'.repeat(32),
            activeVersion: '7',
            observedAt: '2026-11-28T00:01:05.000Z',
            expiresAt: '2026-11-28T00:02:05.000Z',
          },
        ],
        nextCursor: null,
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

describe('Gray Match console pages', () => {
  test('edits entry rules, validates via Native Validator preview, and saves draft with If-Match', async () => {
    const fetchMock = installGrayApiMock({ draftMissingOnce: true })
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/gray'] })
    const user = userEvent.setup()

    render(<RouterProvider router={router} />)

    expect(await screen.findByRole('heading', { name: '入口灰度规则' })).toBeTruthy()
    expect(screen.getByText('以 Release 激活证据为准')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '+ 添加入口规则' }))
    const ratioInput = screen.getByLabelText('规则 1 灰度万分比') as HTMLInputElement
    fireEvent.change(ratioInput, { target: { value: '1250' } })
    expect(screen.getByText('12.50%')).toBeTruthy()

    const cidrInput = screen.getByLabelText('规则 1 CIDR 列表')
    fireEvent.change(cidrInput, { target: { value: '10.0.0.0/8\n2001:db8::/32' } })

    await user.click(screen.getByRole('button', { name: '校验并预览 Wire JSON' }))
    expect(await screen.findByText(/Native Validator：已通过/u)).toBeTruthy()
    expect(screen.getByText(/目标 Data ID：ploto\.unified-access\.gray-match/u)).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '保存草稿' }))
    expect(await screen.findByText(/草稿已保存（锁版本 1）/u)).toBeTruthy()

    const saveCall = fetchMock.mock.calls.find(
      ([url, init]) => url === '/api/gray/draft' && init?.method === 'PUT',
    )
    expect(saveCall).toBeTruthy()
    expect(new Headers(saveCall?.[1]?.headers).get('If-Match')).toBe('"0"')
    expect(JSON.parse(String(saveCall?.[1]?.body))).toEqual({
      document: {
        schemaVersion: 1,
        rules: [{ entry: 'internet', ratio: 1250, cidrs: ['10.0.0.0/8', '2001:db8::/32'] }],
      },
    })
  })

  test('creates immutable Release, triggers Rollback as new Release, and inspects activation evidence', async () => {
    const fetchMock = installGrayApiMock()
    vi.spyOn(window, 'confirm').mockReturnValue(true)
    const router = createMemoryRouter(appRoutes, { initialEntries: ['/gray/releases'] })
    const user = userEvent.setup()

    render(<RouterProvider router={router} />)

    expect(await screen.findByText('R12')).toBeTruthy()
    expect(await screen.findByText('草稿可发布')).toBeTruthy()

    await user.type(screen.getByLabelText('灰度发布说明'), '放量至 5%')
    await user.click(screen.getByRole('button', { name: '从草稿创建 Release' }))
    expect(await screen.findByText('R13')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: '回滚到此版本' }))
    expect(await screen.findByText('R14')).toBeTruthy()

    const rollbackCall = fetchMock.mock.calls.find(
      ([url, init]) =>
        url === `/api/gray/releases/${releaseId}/rollback` && init?.method === 'POST',
    )
    expect(rollbackCall).toBeTruthy()

    const evidenceButtons = screen.getAllByRole('button', { name: '查看实例证据' })
    await user.click(evidenceButtons.at(-1)!)
    expect(await screen.findByText('access-gray-0')).toBeTruthy()
    expect(screen.getByText('候选：accepted')).toBeTruthy()
  })
})
