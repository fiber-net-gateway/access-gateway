import { useCallback, useEffect, useMemo, useState } from 'react'

import {
  createProject,
  createRelease,
  fetchConfigurationVersion,
  fetchConfigurationVersions,
  fetchCurrentConfigurationVersion,
  fetchHealth,
  fetchProjectReleases,
  fetchProjects,
  fetchRelease,
  fetchSystemStatus,
  queueReleasePublication,
  restoreConfigurationVersion,
  saveConfigurationVersion,
  validateProjectRoutes,
} from './api/client'
import type {
  ApiConnectionState,
  ConfigurationVersionDetail,
  ConfigurationVersionSummary,
  HealthResponse,
  ProjectReleaseView,
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

const terminalReleaseStatuses = new Set([
  'published',
  'partially_published',
  'publish_failed',
  'validation_failed',
  'canceled',
  'superseded',
  'abandoned',
])

export default function App() {
  const [apiState, setApiState] = useState<ApiConnectionState>('loading')
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [systemStatus, setSystemStatus] = useState<SystemStatusResponse | null>(null)
  const [projects, setProjects] = useState<readonly ProjectView[]>([])
  const [selectedProjectId, setSelectedProjectId] = useState<string | null>(null)
  const [model, setModel] = useState<ProjectRoutesModel>(initialRouteModel)
  const [savedFingerprint, setSavedFingerprint] = useState(modelFingerprint(initialRouteModel()))
  const [versions, setVersions] = useState<readonly ConfigurationVersionSummary[]>([])
  const [currentVersionId, setCurrentVersionId] = useState<string | null>(null)
  const [configurationLockVersion, setConfigurationLockVersion] = useState('0')
  const [releases, setReleases] = useState<readonly ProjectReleaseView[]>([])
  const [previewVersion, setPreviewVersion] = useState<ConfigurationVersionDetail | null>(null)
  const [routeLoading, setRouteLoading] = useState(false)
  const [saving, setSaving] = useState(false)
  const [validating, setValidating] = useState(false)
  const [publishing, setPublishing] = useState(false)
  const [validation, setValidation] = useState<ProjectRoutesValidationView | null>(null)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const selectedProject = useMemo(
    () => projects.find((project) => project.id === selectedProjectId) ?? null,
    [projects, selectedProjectId],
  )
  const hasUnsavedChanges = !routeLoading && modelFingerprint(model) !== savedFingerprint

  const confirmDiscardChanges = useCallback((): boolean => {
    return !hasUnsavedChanges || window.confirm('当前工作区 YAML 尚未保存为版本，确定放弃吗？')
  }, [hasUnsavedChanges])

  const loadProjects = useCallback(async (signal?: AbortSignal) => {
    const items = await fetchProjects(signal)
    setProjects(items)
    setSelectedProjectId((current) =>
      current && items.some((item) => item.id === current) ? current : (items[0]?.id ?? null),
    )
    return items
  }, [])

  const loadWorkspace = useCallback(async (projectId: string, signal?: AbortSignal) => {
    setRouteLoading(true)
    try {
      const [versionList, current, releaseList] = await Promise.all([
        fetchConfigurationVersions(projectId, signal),
        fetchCurrentConfigurationVersion(projectId, signal),
        fetchProjectReleases(projectId, signal),
      ])
      const next = current?.version.model ?? initialRouteModel()
      setVersions(versionList.items)
      setCurrentVersionId(versionList.currentVersionId)
      setConfigurationLockVersion(versionList.lockVersion)
      setReleases(releaseList)
      setModel(next)
      setSavedFingerprint(modelFingerprint(next))
      setValidation(null)
      setPreviewVersion(null)
      setErrorMessage(null)
    } finally {
      setRouteLoading(false)
    }
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
    if (!selectedProjectId) {
      const empty = initialRouteModel()
      setModel(empty)
      setSavedFingerprint(modelFingerprint(empty))
      setVersions([])
      setCurrentVersionId(null)
      setConfigurationLockVersion('0')
      setReleases([])
      return
    }
    const controller = new AbortController()
    void loadWorkspace(selectedProjectId, controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载配置版本失败')
      }
    })
    return () => controller.abort()
  }, [loadWorkspace, selectedProjectId])

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
    if (projectId !== selectedProjectId && confirmDiscardChanges()) setSelectedProjectId(projectId)
  }

  const handleCreateProject = async (domain: string): Promise<void> => {
    if (!confirmDiscardChanges()) throw new Error('已取消创建，工作区 YAML 尚未保存为版本')
    const created = await createProject(domain)
    await loadProjects()
    setSelectedProjectId(created.id)
  }

  const handleModelChange = (next: ProjectRoutesModel): void => {
    setModel(next)
    setValidation(null)
  }

  const refreshVersionList = async (projectId: string): Promise<void> => {
    const result = await fetchConfigurationVersions(projectId)
    setVersions(result.items)
    setCurrentVersionId(result.currentVersionId)
    setConfigurationLockVersion(result.lockVersion)
  }

  const handleSaveRoutes = async (changeSummary: string): Promise<void> => {
    if (!selectedProject) throw new Error('请选择一个域名 Project')
    if (!hasUnsavedChanges) throw new Error('当前工作区没有需要保存的修改')
    setSaving(true)
    try {
      const saved = await saveConfigurationVersion(
        selectedProject.id,
        configurationLockVersion,
        currentVersionId,
        changeSummary,
        model,
      )
      setModel(saved.version.model)
      setSavedFingerprint(modelFingerprint(saved.version.model))
      setCurrentVersionId(saved.version.id)
      setConfigurationLockVersion(saved.lockVersion)
      setValidation(null)
      await Promise.all([refreshVersionList(selectedProject.id), loadProjects()])
      setErrorMessage(null)
    } finally {
      setSaving(false)
    }
  }

  const handleRestoreVersion = async (versionId: string, number: number): Promise<void> => {
    if (!selectedProject || !currentVersionId) throw new Error('当前项目还没有可恢复的配置版本')
    if (hasUnsavedChanges && !window.confirm('恢复会丢弃当前未保存的工作区修改，是否继续？')) return
    const saved = await restoreConfigurationVersion(
      selectedProject.id,
      versionId,
      currentVersionId,
      configurationLockVersion,
      `从 V${number} 恢复`,
    )
    setModel(saved.version.model)
    setSavedFingerprint(modelFingerprint(saved.version.model))
    setCurrentVersionId(saved.version.id)
    setConfigurationLockVersion(saved.lockVersion)
    setValidation(null)
    setPreviewVersion(null)
    await Promise.all([refreshVersionList(selectedProject.id), loadProjects()])
  }

  const handleViewVersion = async (versionId: string): Promise<void> => {
    if (!selectedProject) return
    setPreviewVersion(await fetchConfigurationVersion(selectedProject.id, versionId))
  }

  const handlePublishVersion = async (
    versionId: string,
    title: string,
    description: string,
  ): Promise<void> => {
    if (!selectedProject || !currentVersionId) throw new Error('请先保存一个配置版本')
    setPublishing(true)
    try {
      const prepared = await createRelease(
        selectedProject.id,
        versionId,
        currentVersionId,
        title,
        description,
      )
      let release = await queueReleasePublication(prepared.id)
      setReleases((items) => [release, ...items.filter((item) => item.id !== release.id)])
      for (
        let attempt = 0;
        attempt < 30 && !terminalReleaseStatuses.has(release.status);
        attempt += 1
      ) {
        await new Promise((resolve) => setTimeout(resolve, 1_000))
        release = await fetchRelease(release.id)
        setReleases((items) => [release, ...items.filter((item) => item.id !== release.id)])
      }
      await Promise.all([
        refreshVersionList(selectedProject.id),
        fetchProjectReleases(selectedProject.id).then(setReleases),
        loadProjects(),
      ])
    } finally {
      setPublishing(false)
    }
  }

  const handleValidateRoutes = async (): Promise<void> => {
    if (!selectedProject) throw new Error('请选择一个域名 Project')
    setValidating(true)
    try {
      setValidation(await validateProjectRoutes(selectedProject.id, model))
      setErrorMessage(null)
    } finally {
      setValidating(false)
    }
  }

  return (
    <AppShell apiState={apiState}>
      <ProjectsPage
        apiState={apiState}
        configurationLockVersion={configurationLockVersion}
        currentVersionId={currentVersionId}
        errorMessage={errorMessage}
        hasUnsavedChanges={hasUnsavedChanges}
        health={health}
        model={model}
        previewVersion={previewVersion}
        projects={projects}
        publishing={publishing}
        releases={releases}
        routeLoading={routeLoading}
        saving={saving}
        selectedProjectId={selectedProjectId}
        systemStatus={systemStatus}
        validating={validating}
        validation={validation}
        versions={versions}
        onClosePreview={() => setPreviewVersion(null)}
        onCreateProject={handleCreateProject}
        onModelChange={handleModelChange}
        onPublishVersion={handlePublishVersion}
        onRestoreVersion={handleRestoreVersion}
        onSaveRoutes={handleSaveRoutes}
        onSelectProject={handleSelectProject}
        onValidateRoutes={handleValidateRoutes}
        onViewVersion={handleViewVersion}
      />
    </AppShell>
  )
}
