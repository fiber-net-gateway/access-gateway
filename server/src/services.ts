import type { AuthService } from './modules/auth/model.js'
import type { DraftService } from './modules/drafts/service.js'
import type { EnvironmentService } from './modules/environments/service.js'
import type { ProjectService } from './modules/projects/service.js'
import type { ReleaseService } from './modules/releases/service.js'
import type { SystemStatusService } from './modules/system/service.js'
import type { ConfigurationVersionService } from './modules/versions/service.js'

export interface ApplicationServices {
  auth: AuthService
  environments: EnvironmentService
  projects: ProjectService
  drafts: DraftService
  versions: ConfigurationVersionService
  releases: ReleaseService
  system: SystemStatusService
}

export interface ApplicationRuntime {
  services: ApplicationServices
  close(): Promise<void>
}
