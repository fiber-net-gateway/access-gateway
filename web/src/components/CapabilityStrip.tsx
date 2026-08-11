import type { ApiConnectionState, HealthResponse, SystemStatusResponse } from '../api/types'

interface CapabilityStripProps {
  apiState: ApiConnectionState
  health: HealthResponse | null
  systemStatus: SystemStatusResponse | null
}

function capabilityStatus(capability: { status: string; detail: string } | undefined): {
  label: string
  tone: 'ready' | 'pending' | 'unknown'
} {
  if (capability?.status === 'ready') return { label: '可用', tone: 'ready' }
  if (capability?.status === 'unavailable') return { label: '不可用', tone: 'pending' }
  return { label: '未配置', tone: 'unknown' }
}

export function CapabilityStrip({ apiState, health, systemStatus }: CapabilityStripProps) {
  const validatorCapability = capabilityStatus(systemStatus?.dependencies.nativeValidator)
  const publicationCapability = capabilityStatus(systemStatus?.dependencies.publicationWorker)

  return (
    <section className="capability-strip" aria-label="系统能力">
      <div>
        <span className={`connection-dot connection-dot-${apiState}`} aria-hidden="true" />
        <span>Console API</span>
        <strong>{apiState === 'online' ? `v${health?.version ?? '—'}` : '连接失败'}</strong>
      </div>
      <div>
        <span>Native Validator</span>
        <span className={`status-chip status-chip-${validatorCapability.tone}`}>
          {validatorCapability.label}
        </span>
      </div>
      <div>
        <span>rnacos 发布</span>
        <span className={`status-chip status-chip-${publicationCapability.tone}`}>
          {publicationCapability.label}
        </span>
      </div>
      <div>
        <span>实例激活</span>
        <span className="status-chip status-chip-unknown">未知</span>
      </div>
    </section>
  )
}
