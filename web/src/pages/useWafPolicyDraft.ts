import { useCallback, useEffect, useState } from 'react'

import {
  ApiClientError,
  createWafPolicyDraft,
  fetchWafPolicyDraft,
  saveWafPolicyDraft,
} from '../api/client'
import type { WafPolicy, WafPolicyDraftView } from '../api/types'

// Shared WAF policy draft workspace: the draft is a per-environment singleton,
// so both the WAF 策略页 and the per-project 策略页 edit the same document
// through If-Match optimistic concurrency.
export interface WafPolicyDraftWorkspace {
  draft: WafPolicyDraftView | null
  policy: WafPolicy | null
  dirty: boolean
  loading: boolean
  saving: boolean
  errorMessage: string | null
  updatePolicy(updater: (current: WafPolicy) => WafPolicy): void
  discard(): void
  save(): Promise<boolean>
  dismissError(): void
}

export function useWafPolicyDraft(): WafPolicyDraftWorkspace {
  const [draft, setDraft] = useState<WafPolicyDraftView | null>(null)
  const [policy, setPolicy] = useState<WafPolicy | null>(null)
  const [savedPolicy, setSavedPolicy] = useState<WafPolicy | null>(null)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const load = useCallback(async (signal?: AbortSignal): Promise<void> => {
    setLoading(true)
    try {
      let view: WafPolicyDraftView
      try {
        view = await fetchWafPolicyDraft(signal)
      } catch (error) {
        if (error instanceof ApiClientError && error.status === 404) {
          view = await createWafPolicyDraft()
        } else {
          throw error
        }
      }
      setDraft(view)
      setPolicy(view.policy)
      setSavedPolicy(view.policy)
      setErrorMessage(null)
    } catch (error) {
      if (!signal?.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 WAF 策略草稿失败')
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

  const updatePolicy = useCallback((updater: (current: WafPolicy) => WafPolicy): void => {
    setPolicy((current) => (current === null ? current : updater(current)))
  }, [])

  const discard = useCallback((): void => {
    setPolicy(savedPolicy)
    setErrorMessage(null)
  }, [savedPolicy])

  const save = useCallback(async (): Promise<boolean> => {
    if (draft === null || policy === null) return false
    setSaving(true)
    try {
      const view = await saveWafPolicyDraft(policy, draft.lockVersion)
      setDraft(view)
      setPolicy(view.policy)
      setSavedPolicy(view.policy)
      setErrorMessage(null)
      return true
    } catch (error) {
      setErrorMessage(
        error instanceof ApiClientError && error.code === 'WAF_POLICY_VERSION_CONFLICT'
          ? '策略草稿已被其他用户更新，本地修改已保留；请刷新后重新应用修改。'
          : error instanceof Error
            ? error.message
            : '保存 WAF 策略草稿失败',
      )
      return false
    } finally {
      setSaving(false)
    }
  }, [draft, policy])

  const dirty =
    policy !== null &&
    savedPolicy !== null &&
    JSON.stringify(policy) !== JSON.stringify(savedPolicy)

  const dismissError = useCallback((): void => setErrorMessage(null), [])

  return {
    draft,
    policy,
    dirty,
    loading,
    saving,
    errorMessage,
    updatePolicy,
    discard,
    save,
    dismissError,
  }
}
