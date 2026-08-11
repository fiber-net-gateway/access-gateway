import { useCallback, useEffect } from 'react'
import { useBeforeUnload, useBlocker } from 'react-router'

const defaultMessage = '当前工作区 YAML 尚未保存为版本，确定放弃吗？'

export function useUnsavedChangesGuard(hasUnsavedChanges: boolean, message = defaultMessage): void {
  useBeforeUnload(
    useCallback(
      (event) => {
        if (!hasUnsavedChanges) return
        event.preventDefault()
        event.returnValue = ''
      },
      [hasUnsavedChanges],
    ),
  )

  const blocker = useBlocker(
    ({ currentLocation, nextLocation }) =>
      hasUnsavedChanges && currentLocation.pathname !== nextLocation.pathname,
  )

  useEffect(() => {
    if (blocker.state !== 'blocked') return
    if (window.confirm(message)) blocker.proceed()
    else blocker.reset()
  }, [blocker, message])
}
