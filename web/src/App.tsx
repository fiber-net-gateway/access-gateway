import { useEffect, useState } from 'react'
import { Outlet, useOutletContext } from 'react-router'

import { fetchHealth, fetchSystemStatus } from './api/client'
import type { ApiConnectionState, HealthResponse, SystemStatusResponse } from './api/types'
import { AppShell } from './components/AppShell'

export interface ConsoleContextValue {
  apiState: ApiConnectionState
  health: HealthResponse | null
  systemStatus: SystemStatusResponse | null
  statusError: string | null
}

export function useConsoleContext(): ConsoleContextValue {
  return useOutletContext<ConsoleContextValue>()
}

export default function App() {
  const [apiState, setApiState] = useState<ApiConnectionState>('loading')
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [systemStatus, setSystemStatus] = useState<SystemStatusResponse | null>(null)
  const [statusError, setStatusError] = useState<string | null>(null)

  useEffect(() => {
    const controller = new AbortController()
    void Promise.all([fetchHealth(controller.signal), fetchSystemStatus(controller.signal)])
      .then(([healthResponse, statusResponse]) => {
        setHealth(healthResponse)
        setSystemStatus(statusResponse)
        setApiState('online')
        setStatusError(null)
      })
      .catch((error: unknown) => {
        if (controller.signal.aborted) return
        setApiState('offline')
        setStatusError(error instanceof Error ? error.message : '控制台连接失败')
      })
    return () => controller.abort()
  }, [])

  return (
    <AppShell apiState={apiState}>
      <Outlet context={{ apiState, health, systemStatus, statusError }} />
    </AppShell>
  )
}
