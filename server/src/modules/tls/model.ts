import type { CertificateFactStatus } from '../certificates/model.js'

export interface TlsSniCertificateSummary {
  id: string
  name: string
  version: number
  status: CertificateFactStatus
  notAfter: string
  fingerprintSha256: string
  runtimeDeploymentStatus: 'unsupported'
}

export interface TlsSniResolutionView {
  serverName: string
  resolutionStatus: 'matched' | 'uncovered' | 'conflict'
  matchKind: 'exact' | 'wildcard' | null
  certificate: TlsSniCertificateSummary | null
  matches: readonly TlsSniCertificateSummary[]
  runtimeDeploymentStatus: 'unsupported'
}
