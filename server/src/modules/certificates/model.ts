export type CertificateFactStatus = 'valid' | 'expiring' | 'expired' | 'superseded'

export interface CertificateVersionView {
  id: string
  version: number
  status: CertificateFactStatus
  subject: string
  issuer: string
  serialNumber: string
  fingerprintSha256: string
  dnsNames: readonly string[]
  notBefore: string
  notAfter: string
  keyType: string
  createdAt: string
}

export interface CertificateView {
  id: string
  name: string
  lockVersion: string
  currentVersion: CertificateVersionView
  versionCount: number
  runtimeDeploymentStatus: 'activation_unknown'
  createdAt: string
  updatedAt: string
}

export interface CreateCertificateInput {
  name: string
  certificatePem: string
  privateKeyPem: string
}

export interface CreateCertificateVersionInput {
  certificatePem: string
  privateKeyPem: string
  confirmSniCoverageChange?: boolean
}

export interface CertificateListResult {
  items: readonly CertificateView[]
}

export interface CertificateVersionListResult {
  items: readonly CertificateVersionView[]
}
