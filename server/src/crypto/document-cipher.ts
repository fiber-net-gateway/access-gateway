import { createCipheriv, createDecipheriv, randomBytes } from 'node:crypto'

import { sha256 } from '../shared/json.js'

const algorithm = 'aes-256-gcm'
const nonceLength = 12
const authTagLength = 16
const dataKeyLength = 32
const wrappedDataKeyLength = nonceLength + authTagLength + dataKeyLength

export interface EncryptedDocument {
  plaintextSha256: Buffer
  plaintextSize: number
  keyId: string
  wrappedDek: Buffer
  nonce: Buffer
  authTag: Buffer
  ciphertext: Buffer
}

export interface StoredEncryptedDocument {
  keyId: string
  wrappedDek: Uint8Array
  nonce: Uint8Array
  authTag: Uint8Array
  ciphertext: Uint8Array
}

export interface DocumentCipher {
  encrypt(plaintext: Uint8Array): EncryptedDocument
  decrypt(document: StoredEncryptedDocument): Buffer
}

function encryptAesGcm(
  key: Uint8Array,
  plaintext: Uint8Array,
): {
  nonce: Buffer
  authTag: Buffer
  ciphertext: Buffer
} {
  const nonce = randomBytes(nonceLength)
  const cipher = createCipheriv(algorithm, key, nonce, { authTagLength })
  const ciphertext = Buffer.concat([cipher.update(plaintext), cipher.final()])
  return { nonce, authTag: cipher.getAuthTag(), ciphertext }
}

function decryptAesGcm(
  key: Uint8Array,
  nonce: Uint8Array,
  authTag: Uint8Array,
  ciphertext: Uint8Array,
): Buffer {
  const decipher = createDecipheriv(algorithm, key, nonce, { authTagLength })
  decipher.setAuthTag(authTag)
  return Buffer.concat([decipher.update(ciphertext), decipher.final()])
}

export class LocalEnvelopeDocumentCipher implements DocumentCipher {
  readonly #keyId: string
  readonly #key: Buffer

  constructor(keyId: string, key: Uint8Array) {
    if (key.length !== dataKeyLength) {
      throw new Error('Document encryption key must be exactly 32 bytes')
    }
    this.#keyId = keyId
    this.#key = Buffer.from(key)
  }

  encrypt(plaintext: Uint8Array): EncryptedDocument {
    const dataKey = randomBytes(dataKeyLength)
    const encrypted = encryptAesGcm(dataKey, plaintext)
    const wrapped = encryptAesGcm(this.#key, dataKey)
    return {
      plaintextSha256: sha256(plaintext),
      plaintextSize: plaintext.byteLength,
      keyId: this.#keyId,
      wrappedDek: Buffer.concat([wrapped.nonce, wrapped.authTag, wrapped.ciphertext]),
      nonce: encrypted.nonce,
      authTag: encrypted.authTag,
      ciphertext: encrypted.ciphertext,
    }
  }

  decrypt(document: StoredEncryptedDocument): Buffer {
    if (document.keyId !== this.#keyId) {
      throw new Error('Document encryption key is unavailable')
    }
    const wrapped = Buffer.from(document.wrappedDek)
    if (wrapped.length !== wrappedDataKeyLength) {
      throw new Error('Encrypted document has an invalid wrapped data key')
    }
    const dataKey = decryptAesGcm(
      this.#key,
      wrapped.subarray(0, nonceLength),
      wrapped.subarray(nonceLength, nonceLength + authTagLength),
      wrapped.subarray(nonceLength + authTagLength),
    )
    return decryptAesGcm(dataKey, document.nonce, document.authTag, document.ciphertext)
  }
}
