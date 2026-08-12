import assert from 'node:assert/strict'
import test from 'node:test'

import { AppError } from '../../shared/errors.js'
import type { Actor } from '../auth/model.js'
import { EnvironmentRepository } from '../environments/repository.js'
import { TlsSniRepository } from './repository.js'
import { DefaultTlsSniService, normalizeSniServerName } from './service.js'

const actor: Actor = {
  internalId: '1',
  publicId: '00000000-0000-4000-8000-000000000001',
  subject: 'test',
  displayName: 'Test Maintainer',
  platformAdmin: false,
}
const environmentId = '00000000-0000-4000-8000-000000000003'

test('normalizes exact ClientHello SNI names without involving Project domains', () => {
  assert.equal(normalizeSniServerName(' API.Example.COM. '), 'api.example.com')
  assert.equal(normalizeSniServerName('例子.测试'), 'xn--fsqu00a.xn--0zwm56d')

  for (const value of [
    '*.example.com',
    '127.0.0.1',
    'https://example.com',
    'example.com:443',
    'bad..example',
  ]) {
    assert.throws(
      () => normalizeSniServerName(value),
      (error: unknown) => error instanceof AppError && error.code === 'INVALID_SNI_SERVER_NAME',
      value,
    )
  }
})

test('resolves ClientHello SNI through the certificate SAN selector repository', async () => {
  const resolvedNames: string[] = []
  const resolution = {
    serverName: 'api.example.com',
    resolutionStatus: 'uncovered' as const,
    matchKind: null,
    certificate: null,
    matches: [],
    runtimeDeploymentStatus: 'activation_unknown' as const,
  }
  const selectors = {
    resolve: async (_environmentInternalId: string, serverName: string) => {
      resolvedNames.push(serverName)
      return resolution
    },
  } as unknown as TlsSniRepository
  const environments = {
    findWorkspace: async () => ({ id: environmentId }),
    internalIdForActor: async () => '3',
  } as unknown as EnvironmentRepository
  const service = new DefaultTlsSniService(selectors, environments)

  assert.equal(await service.resolve(actor, ' API.Example.COM. '), resolution)
  assert.deepEqual(resolvedNames, ['api.example.com'])
})
