import { useCallback, useEffect, useState } from 'react'

import {
  ApiClientError,
  createWafEdgeFunctionsDraft,
  fetchWafEdgeFunctionsDraft,
  saveWafEdgeFunctionsDraft,
} from '../api/client'
import type { WafEdgeFunctionsDraft, WafEdgeFunctionsDraftView } from '../api/types'

// Handwritten edge-function draft workspace: a per-environment singleton like
// the WAF policy draft, saved through If-Match optimistic concurrency. The
// draft only authors handwritten functions — the generated WAF function is
// merged in at release time by the server.
export interface EdgeFunctionsDraftWorkspace {
  draft: WafEdgeFunctionsDraftView | null
  functions: WafEdgeFunctionsDraft['functions']
  dirty: boolean
  loading: boolean
  saving: boolean
  errorMessage: string | null
  updateFunctions(
    updater: (current: WafEdgeFunctionsDraft['functions']) => WafEdgeFunctionsDraft['functions'],
  ): void
  discard(): void
  save(): Promise<boolean>
  dismissError(): void
}

export function useEdgeFunctionsDraft(): EdgeFunctionsDraftWorkspace {
  const [draft, setDraft] = useState<WafEdgeFunctionsDraftView | null>(null)
  const [functions, setFunctions] = useState<WafEdgeFunctionsDraft['functions']>([])
  const [savedFunctions, setSavedFunctions] = useState<WafEdgeFunctionsDraft['functions']>([])
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const load = useCallback(async (signal?: AbortSignal): Promise<void> => {
    setLoading(true)
    try {
      let view: WafEdgeFunctionsDraftView
      try {
        view = await fetchWafEdgeFunctionsDraft(signal)
      } catch (error) {
        if (error instanceof ApiClientError && error.status === 404) {
          view = await createWafEdgeFunctionsDraft()
        } else {
          throw error
        }
      }
      setDraft(view)
      setFunctions(view.draft.functions)
      setSavedFunctions(view.draft.functions)
      setErrorMessage(null)
    } catch (error) {
      if (!signal?.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载边缘函数草稿失败')
      }
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    void load(controller.signal)
    return () => controller.abort()
  }, [load])

  const updateFunctions = useCallback(
    (
      updater: (current: WafEdgeFunctionsDraft['functions']) => WafEdgeFunctionsDraft['functions'],
    ): void => {
      setFunctions((current) => updater(current))
    },
    [],
  )

  const discard = useCallback((): void => {
    setFunctions(savedFunctions)
    setErrorMessage(null)
  }, [savedFunctions])

  const save = useCallback(async (): Promise<boolean> => {
    if (draft === null) return false
    setSaving(true)
    try {
      const view = await saveWafEdgeFunctionsDraft(
        { schemaVersion: 1, functions },
        draft.lockVersion,
      )
      setDraft(view)
      setFunctions(view.draft.functions)
      setSavedFunctions(view.draft.functions)
      setErrorMessage(null)
      return true
    } catch (error) {
      setErrorMessage(
        error instanceof ApiClientError && error.code === 'EDGE_FUNCTION_DRAFT_VERSION_CONFLICT'
          ? '函数草稿已被其他用户更新，本地修改已保留；请刷新后重新应用修改。'
          : error instanceof Error
            ? error.message
            : '保存边缘函数草稿失败',
      )
      return false
    } finally {
      setSaving(false)
    }
  }, [draft, functions])

  // Property order is stable (both sides come from the same shape), so a
  // JSON comparison is sufficient for the dirty flag.
  const dirty = JSON.stringify(functions) !== JSON.stringify(savedFunctions)

  const dismissError = useCallback((): void => setErrorMessage(null), [])

  return {
    draft,
    functions,
    dirty,
    loading,
    saving,
    errorMessage,
    updateFunctions,
    discard,
    save,
    dismissError,
  }
}
