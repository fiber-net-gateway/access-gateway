import { useCallback, useEffect, useMemo, useState } from 'react'

import {
  createProject,
  fetchCurrentDraftRevision,
  fetchHealth,
  fetchProjects,
  fetchSystemStatus,
  saveDraftRevision,
  validateProjectRoutes,
} from './api/client'
import type {
  ApiConnectionState,
  HealthResponse,
  ProjectRoutesModel,
  ProjectRoutesValidationView,
  ProjectView,
  SystemStatusResponse,
} from './api/types'
import { AppShell } from './components/AppShell'
import { ProjectsPage } from './pages/ProjectsPage'
import { initialRouteModel } from './routes/model'

function modelFingerprint(model: ProjectRoutesModel): string {
  return JSON.stringify(model)
}

export default function App() {
  const [apiState, setApiState] = useState<ApiConnectionState>('loading')
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [systemStatus, setSystemStatus] = useState<SystemStatusResponse | null>(null)
  const [projects, setProjects] = useState<readonly ProjectView[]>([])
  const [selectedProjectId, setSelectedProjectId] = useState<string | null>(null)
  const [model, setModel] = useState<ProjectRoutesModel>(initialRouteModel)
  const [savedFingerprint, setSavedFingerprint] = useState(modelFingerprint(initialRouteModel()))
  const [routeLoading, setRouteLoading] = useState(false)
  const [saving, setSaving] = useState(false)
  const [validating, setValidating] = useState(false)
  const [validation, setValidation] = useState<ProjectRoutesValidationView | null>(null)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const selectedProject = useMemo(
    () => projects.find((project) => project.id === selectedProjectId) ?? null,
    [projects, selectedProjectId],
  )
  const hasUnsavedChanges = !routeLoading && modelFingerprint(model) !== savedFingerprint

  const confirmDiscardChanges = useCallback((): boolean => {
    return !hasUnsavedChanges || window.confirm('当前 YAML Route 尚未保存，确定放弃这些修改吗？')
  }, [hasUnsavedChanges])

  const loadProjects = useCallback(async (signal?: AbortSignal) => {
    const items = await fetchProjects(signal)
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
      loadProjects(controller.signal),
    ])
      .then(([healthResponse, statusResponse]) => {
        setHealth(healthResponse)
        setSystemStatus(statusResponse)
        setApiState('online')
        setErrorMessage(null)
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
      const empty = initialRouteModel()
      setModel(empty)
      setSavedFingerprint(modelFingerprint(empty))
      setValidation(null)
      return
    }
    const controller = new AbortController()
    setRouteLoading(true)
    void fetchCurrentDraftRevision(selectedProject.draft.id, controller.signal)
      .then((revision) => {
        const next = revision?.model ?? initialRouteModel()
        setModel(next)
        setSavedFingerprint(modelFingerprint(next))
        setValidation(null)
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
  }, [selectedProject?.draft?.id, selectedProject?.draft?.revision])

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
    if (!confirmDiscardChanges()) {
      throw new Error('已取消创建，当前 YAML Route 尚未保存')
    }
    const created = await createProject(domain)
    await loadProjects()
    setSelectedProjectId(created.id)
  }

  const handleModelChange = (next: ProjectRoutesModel): void => {
    setModel(next)
    setValidation(null)
  }

  const handleSaveRoutes = async (): Promise<void> => {
    if (!selectedProject?.draft) throw new Error('请选择一个域名 Project')
    if (!hasUnsavedChanges) return
    setSaving(true)
    try {
      await saveDraftRevision(selectedProject.draft.id, selectedProject.draft.lockVersion, model)
      setSavedFingerprint(modelFingerprint(model))
      setErrorMessage(null)
      await loadProjects()
    } finally {
      setSaving(false)
    }
  }

  const handleValidateRoutes = async (): Promise<void> => {
    if (!selectedProject) throw new Error('请选择一个域名 Project')
    setValidating(true)
    try {
      const result = await validateProjectRoutes(selectedProject.id, model)
      setValidation(result)
      setErrorMessage(null)
    } finally {
      setValidating(false)
    }
  }

  return (
    <AppShell apiState={apiState}>
      <ProjectsPage
        apiState={apiState}
        errorMessage={errorMessage}
        hasUnsavedChanges={hasUnsavedChanges}
        health={health}
        model={model}
        projects={projects}
        routeLoading={routeLoading}
        saving={saving}
        selectedProjectId={selectedProjectId}
        systemStatus={systemStatus}
        validating={validating}
        validation={validation}
        onCreateProject={handleCreateProject}
        onModelChange={handleModelChange}
        onSaveRoutes={handleSaveRoutes}
        onSelectProject={handleSelectProject}
        onValidateRoutes={handleValidateRoutes}
      />
    </AppShell>
  )
}
