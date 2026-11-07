import assert from 'node:assert/strict'
import test from 'node:test'

import type { ActivationEvidence } from '../../integrations/activation-evidence/model.js'
import { decideInstanceActivation, type ActivationReleaseResource } from './decision.js'

function evidence(): ActivationEvidence {
  const resource = {
    dataId: 'projects',
    group: 'ACCESS-SERVER',
    candidateStatus: 'accepted' as const,
    observedMd5: '11111111111111111111111111111111',
    activeMd5: '11111111111111111111111111111111',
    observedAtUnixMillis: 1,
    activeAtUnixMillis: 1,
    failure: null,
  }
  return {
    contractVersion: 1,
    evidenceRevision: '7',
    instance: {
      id: 'access-0',
      buildVersion: 'test',
      buildRevision: 'revision',
      startedAtUnixMillis: 1,
    },
    runtime: { state: 'running' },
    routeSnapshot: {
      generation: '3',
      fingerprintSha256: 'a'.repeat(64),
      publishedAtUnixMillis: 1,
      publicationMode: 'atomic_request_pin',
    },
    accessConfig: {
      watcherState: 'running',
      readinessState: 'ready',
      projectList: resource,
    },
    projects: [
      {
        name: 'example.com',
        dataId: 'route.example.com',
        group: 'ACCESS-SERVER',
        subscriptionState: 'subscribed',
        candidateStatus: 'accepted',
        observedMd5: '22222222222222222222222222222222',
        observedVersion: 9,
        activeMd5: '22222222222222222222222222222222',
        activeVersion: 9,
        activeSnapshotGeneration: '3',
        activeLoaded: true,
        observedAtUnixMillis: 1,
        activeAtUnixMillis: 1,
        failure: null,
      },
    ],
    gray: { watcherState: 'running', resource, generation: '1', ruleCount: 0 },
    tls: { enabled: false, watcherState: 'disabled', resource, version: '0', certificateCount: 0 },
    discovery: {
      clientState: 'running',
      configServiceState: 'running',
      namingServiceState: 'running',
      readyServices: 1,
      selectableEndpoints: 1,
      logicalClusters: 1,
      selectorLeases: 0,
    },
  }
}

const resources: ActivationReleaseResource[] = [
  {
    kind: 'project_route',
    dataId: 'route.example.com',
    group: 'ACCESS-SERVER',
    operation: 'upsert',
    verifiedNacosMd5: '22222222222222222222222222222222',
    verifiedSha256: null,
    allocatedProjectVersion: 9,
    projectName: 'example.com',
  },
  {
    kind: 'project_list',
    dataId: 'projects',
    group: 'ACCESS-SERVER',
    operation: 'upsert',
    verifiedNacosMd5: '11111111111111111111111111111111',
    verifiedSha256: null,
    allocatedProjectVersion: null,
    projectName: null,
  },
]

test('activation requires every release resource to be active at the exact digest and version', () => {
  assert.equal(
    decideInstanceActivation({ pollErrorCode: null, evidence: evidence(), resources }),
    'active',
  )

  const stale = evidence()
  stale.projects[0]!.activeVersion = 8
  assert.equal(
    decideInstanceActivation({ pollErrorCode: null, evidence: stale, resources }),
    'pending',
  )
})

test('activation distinguishes explicit rejection and collection failure from pending evidence', () => {
  const rejected = evidence()
  rejected.projects[0]!.activeMd5 = '33333333333333333333333333333333'
  rejected.projects[0]!.candidateStatus = 'rejected'
  rejected.projects[0]!.failure = {
    stage: 'decode',
    code: 'invalid_json',
    field: '',
    offset: 2,
    observedAtUnixMillis: 2,
  }
  assert.equal(
    decideInstanceActivation({ pollErrorCode: null, evidence: rejected, resources }),
    'degraded',
  )
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: 'ACTIVATION_TRANSPORT_UNAVAILABLE',
      evidence: null,
      resources,
    }),
    'degraded',
  )
})

test('an accepted candidate is pending until the immutable active snapshot changes', () => {
  const pending = evidence()
  pending.projects[0]!.activeMd5 = '33333333333333333333333333333333'
  assert.equal(
    decideInstanceActivation({ pollErrorCode: null, evidence: pending, resources }),
    'pending',
  )
})

test('activation verifies resource identity and proves Project removal by full-snapshot absence', () => {
  const wrongIdentity = evidence()
  wrongIdentity.projects[0]!.group = 'WRONG_GROUP'
  assert.equal(
    decideInstanceActivation({ pollErrorCode: null, evidence: wrongIdentity, resources }),
    'degraded',
  )

  const decommissioned = evidence()
  decommissioned.projects = []
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: decommissioned,
      resources: [
        {
          kind: 'project_route',
          dataId: 'route.example.com',
          group: 'ACCESS-SERVER',
          operation: 'remove',
          verifiedNacosMd5: null,
          verifiedSha256: null,
          allocatedProjectVersion: null,
          projectName: 'example.com',
        },
        resources[1]!,
      ],
    }),
    'active',
  )
})

test('watcher failure degrades an unmatched target without denying an exact active snapshot', () => {
  const failed = evidence()
  failed.accessConfig.watcherState = 'failed'
  assert.equal(
    decideInstanceActivation({ pollErrorCode: null, evidence: failed, resources }),
    'active',
  )

  failed.projects[0]!.activeMd5 = '33333333333333333333333333333333'
  assert.equal(
    decideInstanceActivation({ pollErrorCode: null, evidence: failed, resources }),
    'degraded',
  )
})

const edgeIndexMd5 = '44444444444444444444444444444444'
const edgeIndexSha256 = 'c'.repeat(64)
const scriptSha256 = 'd'.repeat(64)

function edgeEvidence() {
  return {
    watcherState: 'running' as const,
    index: {
      dataId: 'ploto.unified-access.edge-functions',
      group: 'ACCESS-SERVER',
      candidateStatus: 'accepted' as const,
      observedMd5: edgeIndexMd5,
      activeMd5: edgeIndexMd5,
      observedAtUnixMillis: 1,
      activeAtUnixMillis: 1,
      failure: null,
    },
    generation: '4',
    functionCount: 1,
    totalScriptBytes: 2048,
    activeIndexSha256: edgeIndexSha256,
  }
}

function edgeResources(script: boolean): ActivationReleaseResource[] {
  const list: ActivationReleaseResource[] = []
  if (script) {
    list.push({
      kind: 'edge_function_script',
      dataId: 'ploto.unified-access.edge-function.waf',
      group: 'ACCESS-SERVER',
      operation: 'upsert',
      verifiedNacosMd5: '55555555555555555555555555555555',
      verifiedSha256: scriptSha256,
      allocatedProjectVersion: 5,
      projectName: null,
    })
  }
  list.push({
    kind: 'edge_function_index',
    dataId: 'ploto.unified-access.edge-functions',
    group: 'ACCESS-SERVER',
    operation: 'upsert',
    verifiedNacosMd5: edgeIndexMd5,
    verifiedSha256: edgeIndexSha256,
    allocatedProjectVersion: 5,
    projectName: null,
  })
  return list
}

test('an edge-function release is active when the live index matches both digests', () => {
  const snapshot = evidence()
  snapshot.edgeFunctions = edgeEvidence()
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: snapshot,
      resources: edgeResources(true),
    }),
    'active',
  )
  // An unmount release proves itself through the empty index alone.
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: snapshot,
      resources: edgeResources(false),
    }),
    'active',
  )
})

test('an edge-function release is pending while the old index snapshot is still live', () => {
  const snapshot = evidence()
  snapshot.edgeFunctions = edgeEvidence()
  snapshot.edgeFunctions.activeIndexSha256 = 'e'.repeat(64)
  snapshot.edgeFunctions.index.activeMd5 = '66666666666666666666666666666666'
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: snapshot,
      resources: edgeResources(true),
    }),
    'pending',
  )
})

test('an edge-function release degrades on watcher failure or rejected published bytes', () => {
  const failedWatcher = evidence()
  failedWatcher.edgeFunctions = edgeEvidence()
  failedWatcher.edgeFunctions.activeIndexSha256 = 'e'.repeat(64)
  failedWatcher.edgeFunctions.watcherState = 'failed'
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: failedWatcher,
      resources: edgeResources(true),
    }),
    'degraded',
  )

  const rejected = evidence()
  rejected.edgeFunctions = edgeEvidence()
  rejected.edgeFunctions.activeIndexSha256 = 'e'.repeat(64)
  rejected.edgeFunctions.index.candidateStatus = 'rejected'
  rejected.edgeFunctions.index.failure = {
    stage: 'assemble',
    code: 'script_compile_failed',
    field: '',
    offset: 0,
    observedAtUnixMillis: 2,
  }
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: rejected,
      resources: edgeResources(true),
    }),
    'degraded',
  )
})

test('an edge-function release stays unknown without edge evidence or on identity mismatch', () => {
  // Builds without edge-function support omit the block entirely; never infer.
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: evidence(),
      resources: edgeResources(true),
    }),
    'unknown',
  )

  const mismatched = evidence()
  mismatched.edgeFunctions = edgeEvidence()
  mismatched.edgeFunctions.index.dataId = 'ploto.unified-access.edge-functions.other'
  assert.equal(
    decideInstanceActivation({
      pollErrorCode: null,
      evidence: mismatched,
      resources: edgeResources(true),
    }),
    'degraded',
  )
})
