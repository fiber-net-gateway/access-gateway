import fastifyStatic from '@fastify/static'
import Fastify, { type FastifyError, type FastifyServerOptions } from 'fastify'

import { UnavailableNativeValidator } from './integrations/native-validator/subprocess.js'
import { FixedActorAuthService } from './modules/auth/service.js'
import { registerDraftRoutes } from './modules/drafts/routes.js'
import { UnavailableDraftService } from './modules/drafts/service.js'
import { registerEnvironmentRoutes } from './modules/environments/routes.js'
import { UnavailableEnvironmentService } from './modules/environments/service.js'
import { registerProjectRoutes } from './modules/projects/routes.js'
import { UnavailableProjectService } from './modules/projects/service.js'
import { registerSystemRoutes } from './modules/system/routes.js'
import { DefaultSystemStatusService } from './modules/system/service.js'
import type { ApplicationServices } from './services.js'
import { AppError, type ErrorField } from './shared/errors.js'

export interface HealthResponse {
  status: 'ok'
  service: 'access-gateway-console-api'
  version: string
}

export interface ApiErrorResponse {
  error: {
    code: string
    message: string
    requestId: string
    fields?: readonly ErrorField[]
  }
}

export interface BuildAppOptions {
  logger?: FastifyServerOptions['logger']
  services?: ApplicationServices
  staticRoot?: string | null
}

const healthResponseSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['status', 'service', 'version'],
  properties: {
    status: { type: 'string', const: 'ok' },
    service: { type: 'string', const: 'access-gateway-console-api' },
    version: { type: 'string' },
  },
} as const

export function buildApp(options: BuildAppOptions = {}) {
  const app = Fastify({
    logger: options.logger ?? false,
    requestIdHeader: 'x-request-id',
  })
  const services = options.services ?? createUnavailableServices()

  if (options.staticRoot) {
    void app.register(fastifyStatic, {
      root: options.staticRoot,
      immutable: true,
      maxAge: '30d',
    })
    app.get('/', async (_request, reply) =>
      reply.sendFile('index.html', { immutable: false, maxAge: 0 }),
    )
  }

  app.get<{ Reply: HealthResponse }>(
    '/api/health',
    {
      schema: {
        response: { 200: healthResponseSchema },
      },
    },
    async () => ({
      status: 'ok',
      service: 'access-gateway-console-api',
      version: '0.1.0',
    }),
  )

  registerSystemRoutes(app, services.system)
  registerEnvironmentRoutes(app, services.auth, services.environments)
  registerProjectRoutes(app, services.auth, services.projects)
  registerDraftRoutes(app, services.auth, services.drafts)

  app.setNotFoundHandler(async (request, reply) => {
    const pathname = request.url.split('?', 1)[0] ?? request.url
    const isApiPath = pathname === '/api' || pathname.startsWith('/api/')
    if (
      options.staticRoot &&
      !isApiPath &&
      (request.method === 'GET' || request.method === 'HEAD')
    ) {
      return reply.sendFile('index.html', { immutable: false, maxAge: 0 })
    }

    const body: ApiErrorResponse = {
      error: {
        code: 'NOT_FOUND',
        message: 'Route not found',
        requestId: String(request.id),
      },
    }
    return reply.code(404).send(body)
  })

  app.setErrorHandler<FastifyError>(async (error, request, reply) => {
    if (error instanceof AppError) {
      const body: ApiErrorResponse = {
        error: {
          code: error.code,
          message: error.message,
          requestId: String(request.id),
          ...(error.fields.length > 0 ? { fields: error.fields } : {}),
        },
      }
      return reply.code(error.statusCode).send(body)
    }

    const isValidationError = error.validation !== undefined
    const isClientError =
      error.statusCode !== undefined && error.statusCode >= 400 && error.statusCode < 500
    const statusCode = isClientError ? error.statusCode! : 500
    if (!isClientError) {
      request.log.error({ err: error }, 'request failed')
    }
    const body: ApiErrorResponse = {
      error: {
        code: isValidationError
          ? 'VALIDATION_ERROR'
          : isClientError
            ? 'BAD_REQUEST'
            : 'INTERNAL_ERROR',
        message: isClientError ? 'Invalid request' : 'Internal server error',
        requestId: String(request.id),
      },
    }
    return reply.code(statusCode).send(body)
  })

  return app
}

function createUnavailableServices(): ApplicationServices {
  const auth = new FixedActorAuthService({
    internalId: '0',
    publicId: '00000000-0000-4000-8000-000000000001',
    subject: 'test-actor',
    displayName: 'Test Actor',
    platformAdmin: true,
  })
  const validator = new UnavailableNativeValidator(1)
  return {
    auth,
    environments: new UnavailableEnvironmentService(),
    projects: new UnavailableProjectService(),
    drafts: new UnavailableDraftService(),
    system: new DefaultSystemStatusService(null, auth, validator),
  }
}
