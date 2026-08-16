import { hostname } from 'node:os'

import type { ActivationCollectorConfig, ActivationTargetConfig } from '../../config/env.js'
import type { DatabasePool } from '../../database/types.js'
import type {
  ActivationEvidence,
  ActivationEvidenceClient,
} from '../../integrations/activation-evidence/model.js'
import { ActivationEvidenceClientError } from '../../integrations/activation-evidence/model.js'
import { ActivationCollectorRepository } from './collector-repository.js'

function stableErrorCode(error: unknown): string {
  return error instanceof ActivationEvidenceClientError
    ? error.code
    : 'ACTIVATION_COLLECTOR_INTERNAL_ERROR'
}

export class ActivationCollector {
  readonly #config: ActivationCollectorConfig
  readonly #client: ActivationEvidenceClient
  readonly #repository: ActivationCollectorRepository
  #synchronized = false
  #nextTarget = 0

  constructor(
    pool: DatabasePool,
    client: ActivationEvidenceClient,
    config: ActivationCollectorConfig,
  ) {
    this.#config = config
    this.#client = client
    this.#repository = new ActivationCollectorRepository(pool, {
      owner: `${hostname()}:${process.pid}`,
      pollIntervalMillis: config.pollIntervalMillis,
      evidenceTtlMillis: config.evidenceTtlMillis,
      leaseMillis: config.leaseMillis,
    })
  }

  async runOnce(): Promise<number> {
    if (!this.#synchronized) {
      await this.#repository.synchronizeTargets(this.#config.targets)
      this.#synchronized = true
    }
    const claims = []
    for (const target of this.rotatedTargets()) {
      const claim = await this.#repository.claim(target)
      if (claim) claims.push(claim)
      if (claims.length === this.#config.concurrency) break
    }
    await Promise.all(
      claims.map(async (claim) => {
        let evidence: ActivationEvidence
        try {
          evidence = await this.#client.collect(claim.target)
        } catch (error) {
          await this.#repository.persistFailure(claim, stableErrorCode(error))
          return
        }
        await this.#repository.persistSuccess(claim, evidence)
      }),
    )
    return claims.length
  }

  private rotatedTargets(): readonly ActivationTargetConfig[] {
    const targets = this.#config.targets
    if (targets.length === 0) return []
    const output = [...targets.slice(this.#nextTarget), ...targets.slice(0, this.#nextTarget)]
    this.#nextTarget = (this.#nextTarget + 1) % targets.length
    return output
  }
}
