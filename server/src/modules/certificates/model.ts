export type CertificateFactStatus = 'valid' | 'expiring' | 'expired' | 'superseded'

export interface CertificateView {
  id: string
  name: string
  status: CertificateFactStatus
  subject: string
  issuer: string
  serialNumber: string
  fingerprintSha256: string
  dnsNames: readonly string[]
  notBefore: string
  notAfter: string
  keyType: string
  bindingCount: number
  runtimeDeploymentStatus: 'unsupported'
  createdAt: string
}

export interface ProjectCertificateBindingView {
  projectId: string
  domain: string
  certificate: CertificateView | null
  coverageStatus: 'covered' | 'unbound'
  runtimeDeploymentStatus: 'unsupported'
  boundAt: string | null
}

export interface CreateCertificateInput {
  name: string
  certificatePem: string
  privateKeyPem: string
}

export interface BindProjectCertificateInput {
  certificateId: string
}

export interface CertificateListResult {
  items: readonly CertificateView[]
}
