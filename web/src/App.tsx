import { useCallback, useEffect, useState } from 'react'

import {
  createEnvironment,
  createProject,
  fetchEnvironments,
  fetchHealth,
  fetchProjects,
  fetchSystemStatus,
} from './api/client'
import type {
  ApiConnectionState,
  CreateEnvironmentInput,
  EnvironmentView,
  HealthResponse,
  ProjectView,
  SystemStatusResponse,
} from './api/types'
import { AppShell } from './components/AppShell'
import { OverviewPage } from './pages/OverviewPage'

export default function App() {
  const [apiState, setApiState] = useState<ApiConnectionState>('loading')
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [systemStatus, setSystemStatus] = useState<SystemStatusResponse | null>(null)
  const [environments, setEnvironments] = useState<readonly EnvironmentView[]>([])
  const [selectedEnvironmentId, setSelectedEnvironmentId] = useState<string | null>(null)
  const [projects, setProjects] = useState<readonly ProjectView[]>([])
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const loadEnvironments = useCallback(async (signal?: AbortSignal) => {
    const items = await fetchEnvironments(signal)
    setEnvironments(items)
    setSelectedEnvironmentId((current) => current ?? items[0]?.id ?? null)
  }, [])

  useEffect(() => {
    const controller = new AbortController()

    void Promise.all([fetchHealth(controller.signal), fetchSystemStatus(controller.signal)])
      .then(([healthResponse, statusResponse]) => {
        setHealth(healthResponse)
        setSystemStatus(statusResponse)
        setApiState('online')
        setErrorMessage(null)
        if (statusResponse.dependencies.database.status === 'ready') {
          void loadEnvironments(controller.signal).catch((error: unknown) => {
            if (!controller.signal.aborted) {
              setErrorMessage(
                error instanceof Error ? error.message : 'Failed to load environments',
              )
            }
          })
        }
      })
      .catch((error: unknown) => {
        if (controller.signal.aborted) {
          return
        }
        setApiState('offline')
        setErrorMessage(error instanceof Error ? error.message : 'Unknown connection error')
      })

    return () => controller.abort()
  }, [loadEnvironments])

  useEffect(() => {
    if (!selectedEnvironmentId) {
      setProjects([])
      return
    }
    const controller = new AbortController()
    void fetchProjects(selectedEnvironmentId, controller.signal)
      .then(setProjects)
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setErrorMessage(error instanceof Error ? error.message : 'Failed to load projects')
        }
      })
    return () => controller.abort()
  }, [selectedEnvironmentId])

  const handleCreateEnvironment = async (input: CreateEnvironmentInput): Promise<void> => {
    const created = await createEnvironment(input)
    await loadEnvironments()
    setSelectedEnvironmentId(created.id)
  }

  const handleCreateProject = async (name: string): Promise<void> => {
    if (!selectedEnvironmentId) return
    await createProject(selectedEnvironmentId, name)
    setProjects(await fetchProjects(selectedEnvironmentId))
  }

  return (
    <AppShell apiState={apiState}>
      <OverviewPage
        apiState={apiState}
        health={health}
        systemStatus={systemStatus}
        environments={environments}
        selectedEnvironmentId={selectedEnvironmentId}
        projects={projects}
        errorMessage={errorMessage}
        onSelectEnvironment={setSelectedEnvironmentId}
        onCreateEnvironment={handleCreateEnvironment}
        onCreateProject={handleCreateProject}
      />
    </AppShell>
  )
}
