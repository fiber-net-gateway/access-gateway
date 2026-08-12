import assert from 'node:assert/strict'
import { generateKeyPairSync, sign } from 'node:crypto'
import test from 'node:test'

import { AppError } from '../../shared/errors.js'
import {
  certificateCoversDomain,
  missingManagedDnsNames,
  parseCertificateUpload,
} from './parser.js'

function derLength(length: number): Buffer {
  if (length < 128) return Buffer.from([length])
  const bytes: number[] = []
  for (let value = length; value > 0; value >>= 8) bytes.unshift(value & 0xff)
  return Buffer.from([0x80 | bytes.length, ...bytes])
}

function der(tag: number, ...parts: readonly Uint8Array[]): Buffer {
  const content = Buffer.concat(parts.map((part) => Buffer.from(part)))
  return Buffer.concat([Buffer.from([tag]), derLength(content.length), content])
}

function oid(value: string): Buffer {
  const parts = value.split('.').map(Number)
  const encoded = [parts[0]! * 40 + parts[1]!]
  for (const part of parts.slice(2)) {
    const bytes = [part & 0x7f]
    for (let value = part >> 7; value > 0; value >>= 7) bytes.unshift(0x80 | (value & 0x7f))
    encoded.push(...bytes)
  }
  return der(0x06, Buffer.from(encoded))
}

function pem(label: string, value: Uint8Array): string {
  const body =
    Buffer.from(value)
      .toString('base64')
      .match(/.{1,64}/gu)
      ?.join('\n') ?? ''
  return `-----BEGIN ${label}-----\n${body}\n-----END ${label}-----\n`
}

function createTestCertificate(): { certificatePem: string; privateKeyPem: string } {
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' })
  const signatureAlgorithm = der(0x30, oid('1.2.840.10045.4.3.2'))
  const commonName = der(
    0x30,
    der(0x31, der(0x30, oid('2.5.4.3'), der(0x0c, Buffer.from('demo.local', 'utf8')))),
  )
  const validity = der(
    0x30,
    der(0x17, Buffer.from('260101000000Z', 'ascii')),
    der(0x17, Buffer.from('281231235959Z', 'ascii')),
  )
  const subjectAlternativeNames = der(
    0x30,
    der(0x82, Buffer.from('demo.local', 'ascii')),
    der(0x82, Buffer.from('localhost', 'ascii')),
  )
  const extensions = der(
    0xa3,
    der(0x30, der(0x30, oid('2.5.29.17'), der(0x04, subjectAlternativeNames))),
  )
  const tbsCertificate = der(
    0x30,
    der(0xa0, der(0x02, Buffer.from([0x02]))),
    der(0x02, Buffer.from([0x01])),
    signatureAlgorithm,
    commonName,
    validity,
    commonName,
    publicKey.export({ type: 'spki', format: 'der' }),
    extensions,
  )
  const signature = sign('sha256', tbsCertificate, privateKey)
  const certificate = der(
    0x30,
    tbsCertificate,
    signatureAlgorithm,
    der(0x03, Buffer.from([0x00]), signature),
  )
  return {
    certificatePem: pem('CERTIFICATE', certificate),
    privateKeyPem: privateKey.export({ type: 'pkcs8', format: 'pem' }).toString(),
  }
}

test('parses matching PEM material into public certificate metadata', () => {
  const material = createTestCertificate()
  const parsed = parseCertificateUpload(
    { name: 'Demo identity', ...material },
    new Date('2026-08-12T00:00:00.000Z'),
  )

  assert.equal(parsed.name, 'Demo identity')
  assert.deepEqual(parsed.dnsNames, ['demo.local', 'localhost'])
  assert.equal(parsed.keyType, 'ec')
  assert.equal(parsed.fingerprintSha256.length, 32)
  assert.ok(!JSON.stringify(parsed.dnsNames).includes('PRIVATE KEY'))
})

test('rejects a private key that does not match the leaf certificate', () => {
  const material = createTestCertificate()
  const { privateKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' })
  const mismatch = privateKey.export({ type: 'pkcs8', format: 'pem' }).toString()

  assert.throws(
    () =>
      parseCertificateUpload(
        { name: 'Mismatch', certificatePem: material.certificatePem, privateKeyPem: mismatch },
        new Date('2026-08-12T00:00:00.000Z'),
      ),
    (error: unknown) =>
      error instanceof AppError &&
      error.code === 'INVALID_CERTIFICATE' &&
      error.fields[0]?.code === 'CERTIFICATE_KEY_MISMATCH',
  )
})

test('uses one-label wildcard DNS SAN coverage and rejects expired uploads', () => {
  assert.equal(certificateCoversDomain(['*.example.com'], 'api.example.com'), true)
  assert.equal(certificateCoversDomain(['*.example.com'], 'a.b.example.com'), false)
  assert.equal(certificateCoversDomain(['*.example.com'], 'example.com'), false)
  const material = createTestCertificate()

  assert.throws(
    () =>
      parseCertificateUpload(
        { name: 'Expired', ...material },
        new Date('2029-01-01T00:00:00.000Z'),
      ),
    (error: unknown) =>
      error instanceof AppError && error.fields[0]?.code === 'CERTIFICATE_EXPIRED',
  )
})

test('requires renewed versions to preserve every managed DNS selector', () => {
  assert.deepEqual(
    missingManagedDnsNames(
      ['api.example.com', '*.internal.example.com'],
      ['api.example.com', '*.internal.example.com'],
    ),
    [],
  )
  assert.deepEqual(
    missingManagedDnsNames(
      ['api.example.com', 'one.internal.example.com'],
      ['api.example.com', '*.internal.example.com'],
    ),
    ['*.internal.example.com'],
  )
})
