import assert from 'node:assert/strict'
import { chmod, mkdtemp, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'

import { SubprocessNativeValidator } from './subprocess.js'

async function validatorFixture(source: string): Promise<{ directory: string; path: string }> {
  const directory = await mkdtemp(join(tmpdir(), 'access-gateway-validator-'))
  const path = join(directory, 'validator.mjs')
  await writeFile(path, `#!/usr/bin/env node\n${source}`, 'utf8')
  await chmod(path, 0o700)
  return { directory, path }
}

test('subprocess validator uses the versioned stdin/stdout protocol', async (context) => {
  const fixture = await validatorFixture(`
let input = ''
process.stdin.setEncoding('utf8')
process.stdin.on('data', (chunk) => { input += chunk })
process.stdin.on('end', () => {
  const request = JSON.parse(input)
  const payload = Buffer.from(request.payloadBase64, 'base64').toString('utf8')
  process.stdout.write(JSON.stringify({
    contractVersion: request.contractVersion,
    valid: payload === 'wire-payload',
    normalized: { projectVersion: 3 },
    errors: [],
  }))
})
`)
  context.after(() => rm(fixture.directory, { recursive: true, force: true }))
  const validator = new SubprocessNativeValidator({
    path: fixture.path,
    contractVersion: 1,
    revision: 'fixture-revision',
    timeoutMillis: 2_000,
    maxInputBytes: 4_096,
    maxOutputBytes: 4_096,
  })

  const result = await validator.validate({
    requestId: '12d7e31d-a3c9-4a46-863a-0edc04f135e7',
    kind: 'project_route',
    project: 'example',
    payload: Buffer.from('wire-payload'),
  })
  assert.equal(result.valid, true)
  assert.equal(result.contractVersion, 1)
  assert.equal(result.validatorRevision, 'fixture-revision')
  assert.deepEqual(result.normalized, { projectVersion: 3 })

  await assert.rejects(
    validator.validate({
      requestId: '6b02981c-c99f-4f46-a27e-bd4f8305f472',
      kind: 'project_route',
      project: 'example',
      payload: Buffer.alloc(4_097),
    }),
    (error: unknown) =>
      typeof error === 'object' &&
      error !== null &&
      (error as { code?: string }).code === 'NATIVE_VALIDATOR_INPUT_LIMIT',
  )
})

test('subprocess validator fails closed on a protocol mismatch', async (context) => {
  const fixture = await validatorFixture(`
process.stdin.resume()
process.stdin.on('end', () => {
  process.stdout.write(JSON.stringify({ contractVersion: 2, valid: true, errors: [] }))
})
`)
  context.after(() => rm(fixture.directory, { recursive: true, force: true }))
  const validator = new SubprocessNativeValidator({
    path: fixture.path,
    contractVersion: 1,
    revision: 'fixture-revision',
    timeoutMillis: 2_000,
    maxInputBytes: 4_096,
    maxOutputBytes: 4_096,
  })

  await assert.rejects(
    validator.validate({
      requestId: 'a8691398-05c6-4412-a466-f706c2ed6c5b',
      kind: 'project_route',
      project: 'example',
      payload: Buffer.from('wire-payload'),
    }),
    (error: unknown) =>
      typeof error === 'object' &&
      error !== null &&
      (error as { code?: string }).code === 'NATIVE_VALIDATOR_PROTOCOL_ERROR',
  )
})
