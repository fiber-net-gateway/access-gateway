import { isIP } from 'node:net'
import { domainToASCII } from 'node:url'

/**
 * Normalize a user supplied host binding to the exact host form consumed by
 * the access-server host map.  Wildcards, IP literals, ports, paths, and
 * other URL-like values intentionally do not belong to this first alias
 * model; callers that need those semantics should use a separate binding
 * type rather than widening this one.
 */
export function normalizeExactHost(value: string): string | null {
  const trimmed = value.trim()
  if (
    trimmed.length === 0 ||
    /[\u0000-\u001f\u007f]/u.test(trimmed) ||
    /[\\/:?#@]/u.test(trimmed)
  ) {
    return null
  }
  const withoutTrailingDot = trimmed.endsWith('.') ? trimmed.slice(0, -1) : trimmed
  if (withoutTrailingDot.length === 0 || withoutTrailingDot.includes('*')) return null
  const ascii = domainToASCII(withoutTrailingDot).toLowerCase()
  if (ascii.length < 1 || ascii.length > 253 || isIP(ascii) !== 0) return null
  const labels = ascii.split('.')
  if (
    labels.length === 0 ||
    labels.some(
      (label) =>
        label.length < 1 || label.length > 63 || !/^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/u.test(label),
    )
  ) {
    return null
  }
  return ascii
}

export function isCanonicalExactHost(value: string): boolean {
  return normalizeExactHost(value) === value
}
