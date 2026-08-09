import { randomUUID } from 'node:crypto'

const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/iu

export function createPublicId(): string {
  return randomUUID()
}

export function publicIdToBuffer(value: string): Buffer {
  if (!uuidPattern.test(value)) {
    throw new Error('Invalid UUID')
  }
  return Buffer.from(value.replaceAll('-', ''), 'hex')
}

export function bufferToPublicId(value: Uint8Array): string {
  const hex = Buffer.from(value).toString('hex')
  if (hex.length !== 32) {
    throw new Error('Invalid binary UUID')
  }
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}

export function isPublicId(value: string): boolean {
  return uuidPattern.test(value)
}
