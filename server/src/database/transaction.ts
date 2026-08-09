import type { DatabasePool, SqlExecutor } from './types.js'

const deadlockErrorNumber = 1213
const lockWaitTimeoutErrorNumber = 1205

interface MysqlErrorLike {
  errno?: unknown
}

export interface TransactionOptions {
  retryOnDeadlock?: boolean
  maxAttempts?: number
}

function isRetryableTransactionError(error: unknown): boolean {
  if (typeof error !== 'object' || error === null) {
    return false
  }
  const errno = (error as MysqlErrorLike).errno
  return errno === deadlockErrorNumber || errno === lockWaitTimeoutErrorNumber
}

function retryDelay(attempt: number): Promise<void> {
  const base = Math.min(10 * 2 ** (attempt - 1), 100)
  const jitter = Math.floor(Math.random() * 10)
  return new Promise((resolve) => setTimeout(resolve, base + jitter))
}

export async function withTransaction<T>(
  pool: DatabasePool,
  operation: (transaction: SqlExecutor) => Promise<T>,
  options: TransactionOptions = {},
): Promise<T> {
  const maxAttempts = options.retryOnDeadlock ? (options.maxAttempts ?? 3) : 1
  for (let attempt = 1; attempt <= maxAttempts; attempt += 1) {
    const connection = await pool.getConnection()
    try {
      await connection.query('SET TRANSACTION ISOLATION LEVEL READ COMMITTED')
      await connection.beginTransaction()
      const result = await operation(connection)
      await connection.commit()
      return result
    } catch (error) {
      await connection.rollback().catch(() => undefined)
      if (attempt < maxAttempts && isRetryableTransactionError(error)) {
        await retryDelay(attempt)
        continue
      }
      throw error
    } finally {
      connection.release()
    }
  }
  throw new Error('Transaction retry loop completed without a result')
}
