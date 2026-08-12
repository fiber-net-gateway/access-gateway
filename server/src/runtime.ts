import { createHash } from 'node:crypto'
import { constants } from 'node:fs'
import { access, readFile } from 'node:fs/promises'

import type { ServerConfig } from './config/env.js'
import { LocalEnvelopeDocumentCipher } from './crypto/document-cipher.js'
import { DocumentRepository } from './crypto/document-repository.js'
import { createDatabasePool } from './database/pool.js'
import { requireCurrentSchema } from './database/schema.js'
import type { DatabasePool } from './database/types.js'
import type { NativeValidator } from './integrations/native-validator/model.js'
import {
  SubprocessNativeValidator,
  UnavailableNativeValidator,
} from './integrations/native-validator/subprocess.js'
import { HttpNacosClient, UnavailableNacosClient } from './integrations/nacos/http.js'
import { AuthRepository } from './modules/auth/repository.js'
import { FixedActorAuthService, UnavailableAuthService } from './modules/auth/service.js'
import { CertificateRepository } from './modules/certificates/repository.js'
import {
  DefaultCertificateService,
  UnavailableCertificateService,
} from './modules/certificates/service.js'
import { DraftRepository } from './modules/drafts/repository.js'
import { DefaultDraftService, UnavailableDraftService } from './modules/drafts/service.js'
import { EnvironmentRepository } from './modules/environments/repository.js'
import {
  DefaultEnvironmentService,
  UnavailableEnvironmentService,
} from './modules/environments/service.js'
import { ProjectRepository } from './modules/projects/repository.js'
import { DefaultProjectService, UnavailableProjectService } from './modules/projects/service.js'
import { ReleaseRepository } from './modules/releases/repository.js'
import { DefaultReleaseService, UnavailableReleaseService } from './modules/releases/service.js'
import { DefaultSystemStatusService } from './modules/system/service.js'
import { ConfigurationVersionRepository } from './modules/versions/repository.js'
import {
  DefaultConfigurationVersionService,
  UnavailableConfigurationVersionService,
} from './modules/versions/service.js'
import type { ApplicationRuntime, ApplicationServices } from './services.js'

async function createNativeValidator(config: ServerConfig): Promise<{
  validator: NativeValidator
  detail: string
}> {
  const path = config.nativeValidator.path
  if (!path) {
    return {
      validator: new UnavailableNativeValidator(config.nativeValidator.contractVersion),
      detail: 'NATIVE_VALIDATOR_PATH is not configured',
    }
  }
  try {
    await access(path, constants.R_OK | constants.X_OK)
    const binary = await readFile(path)
    const revision = createHash('sha256').update(binary).digest('hex').slice(0, 64)
    return {
      validator: new SubprocessNativeValidator({
        path,
        revision,
        contractVersion: config.nativeValidator.contractVersion,
        timeoutMillis: config.nativeValidator.timeoutMillis,
        maxInputBytes: config.nativeValidator.maxInputBytes,
        maxOutputBytes: config.nativeValidator.maxOutputBytes,
      }),
      detail: `configured revision ${revision}`,
    }
  } catch {
    return {
      validator: new UnavailableNativeValidator(config.nativeValidator.contractVersion),
      detail: 'configured Native Validator binary cannot be read',
    }
  }
}

function unavailableServices(
  config: ServerConfig,
  validator: NativeValidator,
  validatorDetail: string,
): ApplicationServices {
  const auth =
    config.auth.mode === 'development'
      ? new FixedActorAuthService({
          internalId: '0',
          publicId: '00000000-0000-4000-8000-000000000001',
          subject: config.auth.developmentSubject,
          displayName: config.auth.developmentDisplayName,
          platformAdmin: true,
        })
      : new UnavailableAuthService()
  return {
    auth,
    certificates: new UnavailableCertificateService(),
    environments: new UnavailableEnvironmentService(),
    projects: new UnavailableProjectService(),
    drafts: new UnavailableDraftService(),
    versions: new UnavailableConfigurationVersionService(),
    releases: new UnavailableReleaseService(),
    system: new DefaultSystemStatusService(
      null,
      auth,
      validator,
      validatorDetail,
      config.nativeValidator.path !== null,
      config.publication.enabled,
    ),
  }
}

export async function createApplicationRuntime(config: ServerConfig): Promise<ApplicationRuntime> {
  const { validator, detail: validatorDetail } = await createNativeValidator(config)
  if (!config.database.enabled) {
    return {
      services: unavailableServices(config, validator, validatorDetail),
      async close() {},
    }
  }
  if (config.auth.mode !== 'development') {
    throw new Error('AUTH_MODE=oidc is not implemented; refusing to start without an auth provider')
  }

  let pool: DatabasePool | null = null
  try {
    pool = await createDatabasePool(config.database)
    await requireCurrentSchema(pool)
    if (!config.documentEncryption.key) {
      throw new Error('Document encryption key is required when MySQL is enabled')
    }
    const authRepository = new AuthRepository(pool)
    const actor = await authRepository.ensureDevelopmentActor(
      config.auth.developmentSubject,
      config.auth.developmentDisplayName,
    )
    const auth = new FixedActorAuthService(actor)
    const environments = new EnvironmentRepository(pool)
    const projects = new ProjectRepository(pool)
    const documents = new DocumentRepository(
      new LocalEnvelopeDocumentCipher(
        config.documentEncryption.keyId,
        config.documentEncryption.key,
      ),
    )
    const drafts = new DraftRepository(pool, documents)
    const versions = new ConfigurationVersionRepository(pool, documents)
    const nacos = config.publication.enabled
      ? new HttpNacosClient({
          timeoutMillis: config.publication.requestTimeoutMillis,
          maxResponseBytes: config.publication.maxResponseBytes,
          endpointOverride: config.publication.endpointOverride,
        })
      : new UnavailableNacosClient()
    const releases = new ReleaseRepository(pool, documents)
    const certificates = new CertificateRepository(pool, documents)
    const services: ApplicationServices = {
      auth,
      certificates: new DefaultCertificateService(certificates, projects, environments),
      environments: new DefaultEnvironmentService(environments),
      projects: new DefaultProjectService(projects, environments),
      drafts: new DefaultDraftService(drafts, projects, environments, validator),
      versions: new DefaultConfigurationVersionService(versions, projects, environments, validator),
      releases: new DefaultReleaseService(
        releases,
        versions,
        projects,
        environments,
        validator,
        nacos,
      ),
      system: new DefaultSystemStatusService(
        pool,
        auth,
        validator,
        validatorDetail,
        config.nativeValidator.path !== null,
        config.publication.enabled,
      ),
    }
    return {
      services,
      async close() {
        await pool?.end()
      },
    }
  } catch (error) {
    await pool?.end()
    throw error
  }
}
