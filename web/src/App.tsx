import { useEffect, useState } from 'react'

import { fetchHealth } from './api/client'
import type { ApiConnectionState, HealthResponse } from './api/types'
import { AppShell } from './components/AppShell'
import { OverviewPage } from './pages/OverviewPage'

export default function App() {
  const [apiState, setApiState] = useState<ApiConnectionState>('loading')
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  useEffect(() => {
    const controller = new AbortController()

    void fetchHealth(controller.signal)
      .then((response) => {
        setHealth(response)
        setApiState('online')
        setErrorMessage(null)
      })
      .catch((error: unknown) => {
        if (controller.signal.aborted) {
          return
        }
        setApiState('offline')
        setErrorMessage(error instanceof Error ? error.message : 'Unknown connection error')
      })

    return () => controller.abort()
  }, [])

  return (
    <AppShell apiState={apiState}>
      <OverviewPage apiState={apiState} health={health} errorMessage={errorMessage} />
    </AppShell>
  )
}
