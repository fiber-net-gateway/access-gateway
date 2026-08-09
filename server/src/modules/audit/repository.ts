import type { SqlExecutor } from '../../database/types.js'
import { createPublicId, publicIdToBuffer } from '../../shared/ids.js'

export interface AppendAuditEventInput {
  environmentInternalId: string | null
  actorInternalId: string | null
  eventType: string
  targetType: string
  targetPublicId: string | null
  requestId: string
  result: 'success' | 'failure'
  summary: Readonly<Record<string, unknown>>
}

export class AuditRepository {
  async append(executor: SqlExecutor, input: AppendAuditEventInput): Promise<void> {
    await executor.execute(
      `INSERT INTO audit_events
        (public_id, environment_id, actor_user_id, event_type, target_type,
         target_public_id, request_id, result, summary_json)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      [
        publicIdToBuffer(createPublicId()),
        input.environmentInternalId,
        input.actorInternalId,
        input.eventType,
        input.targetType,
        input.targetPublicId ? publicIdToBuffer(input.targetPublicId) : null,
        input.requestId,
        input.result,
        JSON.stringify(input.summary),
      ],
    )
  }
}
