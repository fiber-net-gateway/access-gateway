interface MysqlErrorLike {
  code?: unknown
  errno?: unknown
}

export function isDuplicateKeyError(error: unknown): boolean {
  if (typeof error !== 'object' || error === null) {
    return false
  }
  const candidate = error as MysqlErrorLike
  return candidate.code === 'ER_DUP_ENTRY' || candidate.errno === 1062
}
