import { spawn } from 'node:child_process'
import { isAbsolute } from 'node:path'

import { badRequest, unavailable } from '../../shared/errors.js'
import type {
  NativeValidationError,
  NativeValidationRequest,
  NativeValidationResult,
  NativeValidator,
} from './model.js'

interface SubprocessValidatorOptions {
  path: string
  contractVersion: number
  revision: string
  timeoutMillis: number
  maxInputBytes: number
  maxOutputBytes: number
}

interface ProtocolResponse {
  contractVersion: number
  valid: boolean
  normalized?: Readonly<Record<string, unknown>>
  errors: readonly NativeValidationError[]
}

function parseResponse(value: string, expectedContract: number): ProtocolResponse {
  let parsed: unknown
  try {
    parsed = JSON.parse(value)
  } catch {
    throw unavailable('NATIVE_VALIDATOR_PROTOCOL_ERROR', 'Native Validator returned malformed JSON')
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw unavailable(
      'NATIVE_VALIDATOR_PROTOCOL_ERROR',
      'Native Validator returned an invalid response',
    )
  }
  const response = parsed as Record<string, unknown>
  if (
    response.contractVersion !== expectedContract ||
    typeof response.valid !== 'boolean' ||
    !Array.isArray(response.errors)
  ) {
    throw unavailable('NATIVE_VALIDATOR_PROTOCOL_ERROR', 'Native Validator contract mismatch')
  }
  const errors: NativeValidationError[] = []
  for (const error of response.errors) {
    if (
      typeof error !== 'object' ||
      error === null ||
      typeof (error as Record<string, unknown>).code !== 'string' ||
      typeof (error as Record<string, unknown>).message !== 'string'
    ) {
      throw unavailable(
        'NATIVE_VALIDATOR_PROTOCOL_ERROR',
        'Native Validator returned invalid errors',
      )
    }
    const item = error as Record<string, unknown>
    if (item.field !== undefined && typeof item.field !== 'string') {
      throw unavailable(
        'NATIVE_VALIDATOR_PROTOCOL_ERROR',
        'Native Validator returned an invalid error field',
      )
    }
    if (
      item.offset !== undefined &&
      (typeof item.offset !== 'number' || !Number.isSafeInteger(item.offset) || item.offset < 0)
    ) {
      throw unavailable(
        'NATIVE_VALIDATOR_PROTOCOL_ERROR',
        'Native Validator returned an invalid error offset',
      )
    }
    errors.push({
      code: item.code as string,
      message: item.message as string,
      ...(typeof item.field === 'string' ? { field: item.field } : {}),
      ...(typeof item.offset === 'number' ? { offset: item.offset } : {}),
    })
  }
  if (
    response.normalized !== undefined &&
    (typeof response.normalized !== 'object' ||
      response.normalized === null ||
      Array.isArray(response.normalized))
  ) {
    throw unavailable(
      'NATIVE_VALIDATOR_PROTOCOL_ERROR',
      'Native Validator returned invalid normalized data',
    )
  }
  return {
    contractVersion: expectedContract,
    valid: response.valid,
    errors,
    ...(response.normalized
      ? { normalized: response.normalized as Readonly<Record<string, unknown>> }
      : {}),
  }
}

export class SubprocessNativeValidator implements NativeValidator {
  readonly available = true
  readonly contractVersion: number
  readonly revision: string
  readonly #path: string
  readonly #timeoutMillis: number
  readonly #maxInputBytes: number
  readonly #maxOutputBytes: number

  constructor(options: SubprocessValidatorOptions) {
    if (!isAbsolute(options.path)) {
      throw new Error('Native Validator path must be absolute')
    }
    this.#path = options.path
    this.contractVersion = options.contractVersion
    this.revision = options.revision
    this.#timeoutMillis = options.timeoutMillis
    this.#maxInputBytes = options.maxInputBytes
    this.#maxOutputBytes = options.maxOutputBytes
  }

  async validate(
    request: NativeValidationRequest,
    signal?: AbortSignal,
  ): Promise<NativeValidationResult> {
    if (signal?.aborted) {
      throw signal.reason ?? new Error('Native Validator request was aborted')
    }
    if (request.payload.byteLength > this.#maxInputBytes) {
      throw badRequest(
        'NATIVE_VALIDATOR_INPUT_LIMIT',
        'Native Validator input exceeded its configured limit',
      )
    }
    const input = `${JSON.stringify({
      contractVersion: this.contractVersion,
      requestId: request.requestId,
      kind: request.kind,
      project: request.project,
      payloadBase64: Buffer.from(request.payload).toString('base64'),
    })}\n`

    const response = await new Promise<ProtocolResponse>((resolve, reject) => {
      const child = spawn(this.#path, [], {
        shell: false,
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true,
      })
      const stdout: Buffer[] = []
      let stdoutBytes = 0
      let stderrBytes = 0
      let finished = false

      const finish = (operation: () => void): void => {
        if (finished) return
        finished = true
        clearTimeout(timeout)
        signal?.removeEventListener('abort', abort)
        operation()
      }
      const terminate = (error: Error): void => {
        child.kill('SIGKILL')
        finish(() => reject(error))
      }
      const abort = (): void => terminate(new Error('Native Validator request was aborted'))
      const timeout = setTimeout(
        () =>
          terminate(
            unavailable(
              'NATIVE_VALIDATOR_TIMEOUT',
              'Native Validator did not finish before its deadline',
            ),
          ),
        this.#timeoutMillis,
      )
      timeout.unref()
      signal?.addEventListener('abort', abort, { once: true })

      child.stdout.on('data', (chunk: Buffer) => {
        stdoutBytes += chunk.length
        if (stdoutBytes > this.#maxOutputBytes) {
          terminate(
            unavailable(
              'NATIVE_VALIDATOR_OUTPUT_LIMIT',
              'Native Validator output exceeded its limit',
            ),
          )
          return
        }
        stdout.push(chunk)
      })
      child.stderr.on('data', (chunk: Buffer) => {
        stderrBytes += chunk.length
        if (stderrBytes > this.#maxOutputBytes) {
          terminate(
            unavailable(
              'NATIVE_VALIDATOR_OUTPUT_LIMIT',
              'Native Validator diagnostics exceeded its limit',
            ),
          )
        }
      })
      child.once('error', () => {
        finish(() =>
          reject(
            unavailable('NATIVE_VALIDATOR_UNAVAILABLE', 'Native Validator could not be started'),
          ),
        )
      })
      child.once('close', (code, closeSignal) => {
        finish(() => {
          if (code !== 0 || closeSignal !== null) {
            reject(unavailable('NATIVE_VALIDATOR_FAILED', 'Native Validator process failed'))
            return
          }
          try {
            resolve(parseResponse(Buffer.concat(stdout).toString('utf8'), this.contractVersion))
          } catch (error) {
            reject(error)
          }
        })
      })
      child.stdin.once('error', () => undefined)
      child.stdin.end(input)
    })

    return {
      ...response,
      validatorRevision: this.revision,
    }
  }
}

export class UnavailableNativeValidator implements NativeValidator {
  readonly available = false
  readonly revision = null
  readonly contractVersion: number

  constructor(contractVersion: number) {
    this.contractVersion = contractVersion
  }

  async validate(): Promise<never> {
    throw unavailable('NATIVE_VALIDATOR_UNCONFIGURED', 'Native Validator is not configured')
  }
}
