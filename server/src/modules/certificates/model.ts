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
  managedDnsNames: readonly string[]
  currentVersion: CertificateVersionView
  versionCount: number
  matchedProjectCount: number
  runtimeDeploymentStatus: 'unsupported'
  createdAt: string
  updatedAt: string
}

export interface ProjectCertificateResolutionView {
  projectId: string
  domain: string
  resolutionStatus: 'matched' | 'uncovered' | 'conflict'
  certificate: CertificateView | null
  matches: readonly CertificateView[]
  runtimeDeploymentStatus: 'unsupported'
}

export interface CreateCertificateInput {
  name: string
  certificatePem: string
  privateKeyPem: string
}

export interface CreateCertificateVersionInput {
  certificatePem: string
  privateKeyPem: string
}

export interface CertificateListResult {
  items: readonly CertificateView[]
}

export interface CertificateVersionListResult {
  items: readonly CertificateVersionView[]
}
