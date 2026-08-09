import { createHash } from 'node:crypto'

function normalize(value: unknown): unknown {
  if (Array.isArray(value)) {
    return value.map(normalize)
  }
  if (typeof value !== 'object' || value === null) {
    return value
  }

  const record = value as Record<string, unknown>
  const normalized: Record<string, unknown> = {}
  for (const key of Object.keys(record).sort()) {
    const item = record[key]
    if (item !== undefined) {
      normalized[key] = normalize(item)
    }
  }
  return normalized
}

export function canonicalJson(value: unknown): string {
  return JSON.stringify(normalize(value))
}

export function sha256(value: string | Uint8Array): Buffer {
  return createHash('sha256').update(value).digest()
}
