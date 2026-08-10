import { useCallback, useEffect, useMemo, useState } from 'react'

import {
  createProject,
  fetchCurrentDraftRevision,
  fetchHealth,
  fetchProjects,
  fetchSystemStatus,
  fetchWorkspace,
  saveDraftRevision,
} from './api/client'
import type {
  ApiConnectionState,
  EnvironmentView,
  HealthResponse,
  ProjectView,
  SystemStatusResponse,
} from './api/types'
import { AppShell } from './components/AppShell'
import { OverviewPage } from './pages/OverviewPage'
import { initialRouteModel, parseRouteModel } from './routes/model'

export default function App() {
  const [apiState, setApiState] = useState<ApiConnectionState>('loading')
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [systemStatus, setSystemStatus] = useState<SystemStatusResponse | null>(null)
  const [workspace, setWorkspace] = useState<EnvironmentView | null>(null)
  const [projects, setProjects] = useState<readonly ProjectView[]>([])
  const [selectedProjectId, setSelectedProjectId] = useState<string | null>(null)
  const [routeDocument, setRouteDocument] = useState('')
  const [savedRouteDocument, setSavedRouteDocument] = useState('')
  const [routeLoading, setRouteLoading] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const selectedProject = useMemo(
    () => projects.find((project) => project.id === selectedProjectId) ?? null,
    [projects, selectedProjectId],
  )
  const hasUnsavedChanges = !routeLoading && routeDocument !== savedRouteDocument

  const confirmDiscardChanges = useCallback((): boolean => {
    return !hasUnsavedChanges || window.confirm('当前路由草稿尚未保存，确定放弃这些修改吗？')
  }, [hasUnsavedChanges])

  const loadProjects = useCallback(async (environmentId: string, signal?: AbortSignal) => {
    const items = await fetchProjects(environmentId, signal)
    setProjects(items)
    setSelectedProjectId((current) =>
      current && items.some((item) => item.id === current) ? current : (items[0]?.id ?? null),
    )
    return items
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    void Promise.all([
      fetchHealth(controller.signal),
      fetchSystemStatus(controller.signal),
      fetchWorkspace(controller.signal),
    ])
      .then(([healthResponse, statusResponse, workspaceResponse]) => {
        setHealth(healthResponse)
        setSystemStatus(statusResponse)
        setWorkspace(workspaceResponse)
        setApiState('online')
        setErrorMessage(null)
        return loadProjects(workspaceResponse.id, controller.signal)
      })
      .catch((error: unknown) => {
        if (controller.signal.aborted) return
        setApiState('offline')
        setErrorMessage(error instanceof Error ? error.message : '控制台连接失败')
      })
    return () => controller.abort()
  }, [loadProjects])

  useEffect(() => {
    if (!selectedProject?.draft) {
      setRouteDocument('')
      setSavedRouteDocument('')
      return
    }
    const controller = new AbortController()
    setRouteLoading(true)
    void fetchCurrentDraftRevision(selectedProject.draft.id, controller.signal)
      .then((revision) => {
        const model = revision?.model ?? initialRouteModel(selectedProject.domain)
        const document = JSON.stringify(model, null, 2)
        setRouteDocument(document)
        setSavedRouteDocument(document)
        setErrorMessage(null)
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : '加载路由草稿失败')
        }
      })
      .finally(() => {
        if (!controller.signal.aborted) setRouteLoading(false)
      })
    return () => controller.abort()
  }, [selectedProject])

  useEffect(() => {
    if (!hasUnsavedChanges) return
    const warnBeforeUnload = (event: BeforeUnloadEvent): void => {
      event.preventDefault()
      event.returnValue = ''
    }
    window.addEventListener('beforeunload', warnBeforeUnload)
    return () => window.removeEventListener('beforeunload', warnBeforeUnload)
  }, [hasUnsavedChanges])

  const handleSelectProject = (projectId: string): void => {
    if (projectId !== selectedProjectId && confirmDiscardChanges()) {
      setSelectedProjectId(projectId)
    }
  }

  const handleCreateProject = async (domain: string): Promise<void> => {
    if (!workspace) throw new Error('固定工作区尚未就绪')
    if (!confirmDiscardChanges()) {
      throw new Error('已取消创建，当前路由草稿尚未保存')
    }
    const created = await createProject(workspace.id, domain)
    await loadProjects(workspace.id)
    setSelectedProjectId(created.id)
  }

  const handleSaveRoutes = async (): Promise<void> => {
    if (!workspace || !selectedProject?.draft) throw new Error('请选择一个域名项目')
    const model = parseRouteModel(routeDocument)
    await saveDraftRevision(selectedProject.draft.id, selectedProject.draft.lockVersion, model)
    setSavedRouteDocument(routeDocument)
    await loadProjects(workspace.id)
  }

  return (
    <AppShell apiState={apiState}>
      <OverviewPage
        apiState={apiState}
        health={health}
        systemStatus={systemStatus}
        workspace={workspace}
        projects={projects}
        selectedProjectId={selectedProjectId}
        routeDocument={routeDocument}
        routeLoading={routeLoading}
        hasUnsavedChanges={hasUnsavedChanges}
        errorMessage={errorMessage}
        onSelectProject={handleSelectProject}
        onCreateProject={handleCreateProject}
        onRouteDocumentChange={setRouteDocument}
        onSaveRoutes={handleSaveRoutes}
      />
    </AppShell>
  )
}
