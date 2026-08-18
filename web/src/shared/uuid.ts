/**
 * Generate a UUID without requiring the newer Crypto.randomUUID API.
 *
 * Some supported browser/runtime combinations expose getRandomValues but do
 * not yet expose randomUUID. Keep the fallback here so API idempotency keys
 * and client-side route IDs behave consistently in those environments.
 */
export function createUuid(): string {
  const cryptoApi = globalThis.crypto
  if (typeof cryptoApi?.randomUUID === 'function') return cryptoApi.randomUUID()

  const bytes = new Uint8Array(16)
  if (typeof cryptoApi?.getRandomValues === 'function') {
    cryptoApi.getRandomValues(bytes)
  } else {
    // This is only a last-resort compatibility path for runtimes without the
    // Web Crypto API. getRandomValues is used whenever the runtime provides it.
    for (let index = 0; index < bytes.length; index += 1) {
      bytes[index] = Math.floor(Math.random() * 256)
    }
  }

  bytes[6] = (bytes[6]! & 0x0f) | 0x40
  bytes[8] = (bytes[8]! & 0x3f) | 0x80
  const hex = [...bytes].map((value) => value.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}
