import { createPrivateKey, createPublicKey, X509Certificate } from 'node:crypto'

import { unprocessable } from '../../shared/errors.js'
import type { CreateCertificateInput } from './model.js'

const certificateBlockPattern = /-----BEGIN CERTIFICATE-----[\s\S]+?-----END CERTIFICATE-----/gu
const privateKeyBlockPattern =
  /-----BEGIN (?:RSA |EC |)?PRIVATE KEY-----[\s\S]+?-----END (?:RSA |EC |)?PRIVATE KEY-----/gu

export interface ParsedCertificateUpload {
  name: string
  certificatePem: string
  privateKeyPem: string
  fingerprintSha256: Buffer
  serialNumber: string
  subject: string
  issuer: string
  dnsNames: readonly string[]
  notBefore: Date
  notAfter: Date
  keyType: string
}

function invalidCertificate(code: string, message: string, path: string): never {
  throw unprocessable('INVALID_CERTIFICATE', 'Certificate upload validation failed', [
    { path, code, message },
  ])
}

function parseDnsNames(certificate: X509Certificate): readonly string[] {
  const names: string[] = []
  const value = certificate.subjectAltName ?? ''
  for (const item of value.split(/,\s*/u)) {
    if (!item.startsWith('DNS:')) continue
    let name = item.slice(4)
    if (name.startsWith('"')) {
      try {
        const parsed: unknown = JSON.parse(name)
        if (typeof parsed === 'string') name = parsed
      } catch {
        continue
      }
    }
    name = name.trim().toLowerCase().replace(/\.$/u, '')
    const hostname = name.startsWith('*.') ? name.slice(2) : name
    const labels = hostname.split('.')
    const valid =
      hostname.length <= 253 &&
      labels.length >= 1 &&
      labels.every(
        (label) =>
          label.length >= 1 &&
          label.length <= 63 &&
          /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/u.test(label),
      )
    if (valid && !names.includes(name)) {
      names.push(name)
    }
  }
  return names.sort()
}

function parseCertificateChain(value: string): X509Certificate[] {
  const blocks = value.match(certificateBlockPattern) ?? []
  const remainder = value.replace(certificateBlockPattern, '').trim()
  if (blocks.length === 0 || blocks.length > 16 || remainder.length > 0) {
    invalidCertificate(
      'INVALID_CERTIFICATE_CHAIN_PEM',
      'certificatePem must contain only one or more PEM certificate blocks',
      'certificatePem',
    )
  }
  try {
    return blocks.map((block) => new X509Certificate(block))
  } catch {
    invalidCertificate(
      'INVALID_CERTIFICATE_CHAIN_PEM',
      'certificatePem contains a malformed certificate',
      'certificatePem',
    )
  }
}

function parsePrivateKey(value: string) {
  const blocks = value.match(privateKeyBlockPattern) ?? []
  if (blocks.length !== 1 || value.replace(privateKeyBlockPattern, '').trim().length > 0) {
    invalidCertificate(
      'INVALID_PRIVATE_KEY_PEM',
      'privateKeyPem must contain exactly one unencrypted PEM private key',
      'privateKeyPem',
    )
  }
  try {
    return createPrivateKey(value)
  } catch {
    invalidCertificate(
      'INVALID_PRIVATE_KEY_PEM',
      'privateKeyPem is malformed or encrypted',
      'privateKeyPem',
    )
  }
}

export function parseCertificateUpload(
  input: CreateCertificateInput,
  now = new Date(),
): ParsedCertificateUpload {
  const name = input.name.trim()
  if (name.length < 1 || name.length > 255 || /[\u0000-\u001f\u007f]/u.test(name)) {
    invalidCertificate('INVALID_CERTIFICATE_NAME', 'name must contain 1-255 characters', 'name')
  }
  const chain = parseCertificateChain(input.certificatePem)
  const fingerprints = chain.map((certificate) => certificate.fingerprint256)
  if (new Set(fingerprints).size !== fingerprints.length) {
    invalidCertificate(
      'DUPLICATE_CERTIFICATE_CHAIN_ENTRY',
      'certificatePem must not contain duplicate certificates',
      'certificatePem',
    )
  }
  const leaf = chain[0]!
  const privateKey = parsePrivateKey(input.privateKeyPem)
  const leafPublicKey = leaf.publicKey.export({ type: 'spki', format: 'der' })
  const privatePublicKey = createPublicKey(privateKey).export({ type: 'spki', format: 'der' })
  if (!leafPublicKey.equals(privatePublicKey)) {
    invalidCertificate(
      'CERTIFICATE_KEY_MISMATCH',
      'The private key does not match the leaf certificate',
      'privateKeyPem',
    )
  }
  for (let index = 0; index + 1 < chain.length; index += 1) {
    const child = chain[index]!
    const issuer = chain[index + 1]!
    if (!child.checkIssued(issuer) || !child.verify(issuer.publicKey)) {
      invalidCertificate(
        'INVALID_CERTIFICATE_CHAIN_ORDER',
        'Certificate blocks must be ordered leaf-first and signed by the next certificate',
        'certificatePem',
      )
    }
  }
  const notBefore = new Date(leaf.validFrom)
  const notAfter = new Date(leaf.validTo)
  if (!Number.isFinite(notBefore.valueOf()) || !Number.isFinite(notAfter.valueOf())) {
    invalidCertificate(
      'INVALID_CERTIFICATE_VALIDITY',
      'The leaf certificate validity period is malformed',
      'certificatePem',
    )
  }
  if (notBefore > now) {
    invalidCertificate(
      'CERTIFICATE_NOT_YET_VALID',
      'The leaf certificate is not valid yet',
      'certificatePem',
    )
  }
  if (notAfter <= now) {
    invalidCertificate('CERTIFICATE_EXPIRED', 'The leaf certificate has expired', 'certificatePem')
  }
  const dnsNames = parseDnsNames(leaf)
  if (dnsNames.length === 0) {
    invalidCertificate(
      'CERTIFICATE_DNS_SAN_REQUIRED',
      'The leaf certificate must contain at least one DNS subject alternative name',
      'certificatePem',
    )
  }
  return {
    name,
    certificatePem: input.certificatePem.trim() + '\n',
    privateKeyPem: input.privateKeyPem.trim() + '\n',
    fingerprintSha256: Buffer.from(leaf.fingerprint256.replaceAll(':', ''), 'hex'),
    serialNumber: leaf.serialNumber.toLowerCase(),
    subject: leaf.subject,
    issuer: leaf.issuer,
    dnsNames,
    notBefore,
    notAfter,
    keyType: privateKey.asymmetricKeyType ?? 'unknown',
  }
}

export function certificateCoversDomain(dnsNames: readonly string[], domain: string): boolean {
  const normalized = domain.toLowerCase().replace(/\.$/u, '')
  return dnsNames.some((name) => {
    if (!name.startsWith('*.')) return name === normalized
    const suffix = name.slice(2)
    return (
      normalized.endsWith(`.${suffix}`) &&
      normalized.split('.').length === suffix.split('.').length + 1
    )
  })
}
