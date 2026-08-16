import type { AuthService } from './modules/auth/model.js'
import type { ActivationService } from './modules/activation/model.js'
import type { CertificateService } from './modules/certificates/service.js'
import type { DraftService } from './modules/drafts/service.js'
import type { EnvironmentService } from './modules/environments/service.js'
import type { ProjectService } from './modules/projects/service.js'
import type { ReleaseService } from './modules/releases/service.js'
import type { SystemStatusService } from './modules/system/service.js'
import type { TlsSniService } from './modules/tls/service.js'
import type { TlsCertificateReleaseService } from './modules/tls/release-service.js'
import type { ConfigurationVersionService } from './modules/versions/service.js'

export interface ApplicationServices {
  auth: AuthService
  activation: ActivationService
  certificates: CertificateService
  environments: EnvironmentService
  projects: ProjectService
  drafts: DraftService
  versions: ConfigurationVersionService
  releases: ReleaseService
  system: SystemStatusService
  tlsSni: TlsSniService
  tlsReleases: TlsCertificateReleaseService
}

export interface ApplicationRuntime {
  services: ApplicationServices
  close(): Promise<void>
}
