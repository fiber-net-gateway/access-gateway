export interface ErrorField {
  path: string
  code: string
  message: string
}

export class AppError extends Error {
  readonly code: string
  readonly statusCode: number
  readonly fields: readonly ErrorField[]

  constructor(
    code: string,
    message: string,
    statusCode: number,
    fields: readonly ErrorField[] = [],
  ) {
    super(message)
    this.name = 'AppError'
    this.code = code
    this.statusCode = statusCode
    this.fields = fields
  }
}

export function badRequest(
  code: string,
  message: string,
  fields: readonly ErrorField[] = [],
): AppError {
  return new AppError(code, message, 400, fields)
}

export function unauthorized(): AppError {
  return new AppError('UNAUTHORIZED', 'Authentication is required', 401)
}

export function forbidden(): AppError {
  return new AppError('FORBIDDEN', 'You do not have permission to perform this operation', 403)
}

export function notFound(resource: string): AppError {
  return new AppError('NOT_FOUND', `${resource} was not found`, 404)
}

export function conflict(code: string, message: string): AppError {
  return new AppError(code, message, 409)
}

export function unprocessable(
  code: string,
  message: string,
  fields: readonly ErrorField[] = [],
): AppError {
  return new AppError(code, message, 422, fields)
}

export function unavailable(code: string, message: string): AppError {
  return new AppError(code, message, 503)
}
