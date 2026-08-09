import Fastify, { type FastifyError, type FastifyServerOptions } from 'fastify'

export interface HealthResponse {
  status: 'ok'
  service: 'access-gateway-console-api'
  version: string
}

export interface ApiErrorResponse {
  error: {
    code: string
    message: string
  }
}

export interface BuildAppOptions {
  logger?: FastifyServerOptions['logger']
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

  app.setNotFoundHandler(async (_request, reply) => {
    const body: ApiErrorResponse = {
      error: {
        code: 'NOT_FOUND',
        message: 'Route not found',
      },
    }
    return reply.code(404).send(body)
  })

  app.setErrorHandler<FastifyError>(async (error, request, reply) => {
    request.log.error({ err: error }, 'request failed')
    const isClientError =
      error.statusCode !== undefined && error.statusCode >= 400 && error.statusCode < 500
    const body: ApiErrorResponse = {
      error: {
        code: error.validation
          ? 'VALIDATION_ERROR'
          : isClientError
            ? 'BAD_REQUEST'
            : 'INTERNAL_ERROR',
        message: isClientError ? 'Invalid request' : 'Internal server error',
      },
    }
    return reply.code(isClientError ? error.statusCode! : 500).send(body)
  })

  return app
}
