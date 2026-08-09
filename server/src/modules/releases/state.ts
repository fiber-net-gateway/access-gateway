import { conflict } from '../../shared/errors.js'

export const releaseStatuses = [
  'creating',
  'validating',
  'validation_failed',
  'ready',
  'queued',
  'publishing',
  'published',
  'partially_published',
  'publish_failed',
  'canceled',
  'superseded',
  'abandoned',
] as const

export type ReleaseStatus = (typeof releaseStatuses)[number]

const transitions: Readonly<Record<ReleaseStatus, readonly ReleaseStatus[]>> = {
  creating: ['validating', 'abandoned'],
  validating: ['ready', 'validation_failed', 'abandoned'],
  validation_failed: [],
  ready: ['queued', 'canceled'],
  queued: ['publishing', 'canceled'],
  publishing: ['published', 'partially_published', 'publish_failed'],
  published: ['superseded'],
  partially_published: [],
  publish_failed: [],
  canceled: [],
  superseded: [],
  abandoned: [],
}

export function canTransitionRelease(from: ReleaseStatus, to: ReleaseStatus): boolean {
  return transitions[from].includes(to)
}

export function requireReleaseTransition(from: ReleaseStatus, to: ReleaseStatus): void {
  if (!canTransitionRelease(from, to)) {
    throw conflict('INVALID_RELEASE_TRANSITION', `Release cannot transition from ${from} to ${to}`)
  }
}
