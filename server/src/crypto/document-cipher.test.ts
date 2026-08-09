import assert from 'node:assert/strict'
import test from 'node:test'

import { LocalEnvelopeDocumentCipher } from './document-cipher.js'

test('document cipher encrypts with a unique data key and decrypts exactly', () => {
  const cipher = new LocalEnvelopeDocumentCipher('test-key', Buffer.alloc(32, 3))
  const plaintext = Buffer.from('{"secret":"value"}', 'utf8')
  const first = cipher.encrypt(plaintext)
  const second = cipher.encrypt(plaintext)

  assert.notDeepEqual(first.ciphertext, second.ciphertext)
  assert.notDeepEqual(first.wrappedDek, second.wrappedDek)
  assert.deepEqual(cipher.decrypt(first), plaintext)
  assert.deepEqual(cipher.decrypt(second), plaintext)
  assert.deepEqual(first.plaintextSha256, second.plaintextSha256)
})

test('document cipher rejects tampered ciphertext and unknown keys', () => {
  const cipher = new LocalEnvelopeDocumentCipher('test-key', Buffer.alloc(32, 4))
  const encrypted = cipher.encrypt(Buffer.from('payload'))
  const tampered = Buffer.from(encrypted.ciphertext)
  tampered[0] = tampered[0]! ^ 1

  assert.throws(() => cipher.decrypt({ ...encrypted, ciphertext: tampered }))
  assert.throws(() => cipher.decrypt({ ...encrypted, keyId: 'other-key' }), /unavailable/u)
})
