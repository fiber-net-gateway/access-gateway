import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise'

import type { SqlExecutor } from '../database/types.js'
import { createPublicId, publicIdToBuffer } from '../shared/ids.js'
import { sha256 } from '../shared/json.js'
import type {
  DocumentCipher,
  EncryptedDocument,
  StoredEncryptedDocument,
} from './document-cipher.js'

interface DocumentRow extends RowDataPacket {
  key_id: string
  wrapped_dek: Buffer
  nonce: Buffer
  auth_tag: Buffer
  ciphertext: Buffer
  plaintext_sha256: Buffer
  plaintext_size: string
}

export interface StoreDocumentInput {
  environmentInternalId: string
  purpose: string
  contentType: string
  schemaVersion: number | null
  encrypted: EncryptedDocument
}

export interface StoredDocumentRecord {
  internalId: string
  publicId: string
  sha256: Buffer
  size: number
}

export class DocumentRepository {
  readonly #cipher: DocumentCipher

  constructor(cipher: DocumentCipher) {
    this.#cipher = cipher
  }

  encrypt(value: Uint8Array): EncryptedDocument {
    return this.#cipher.encrypt(value)
  }

  async insert(executor: SqlExecutor, input: StoreDocumentInput): Promise<StoredDocumentRecord> {
    const publicId = createPublicId()
    const [result] = await executor.execute<ResultSetHeader>(
      `INSERT INTO config_documents
        (public_id, environment_id, purpose, content_type, schema_version,
         plaintext_sha256, plaintext_size, key_id, wrapped_dek, nonce, auth_tag, ciphertext)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      [
        publicIdToBuffer(publicId),
        input.environmentInternalId,
        input.purpose,
        input.contentType,
        input.schemaVersion,
        input.encrypted.plaintextSha256,
        input.encrypted.plaintextSize,
        input.encrypted.keyId,
        input.encrypted.wrappedDek,
        input.encrypted.nonce,
        input.encrypted.authTag,
        input.encrypted.ciphertext,
      ],
    )
    return {
      internalId: result.insertId.toString(),
      publicId,
      sha256: input.encrypted.plaintextSha256,
      size: input.encrypted.plaintextSize,
    }
  }

  async decryptByInternalId(executor: SqlExecutor, internalId: string): Promise<Buffer | null> {
    const [rows] = await executor.execute<DocumentRow[]>(
      `SELECT key_id, wrapped_dek, nonce, auth_tag, ciphertext, plaintext_sha256, plaintext_size
       FROM config_documents
       WHERE id = ?`,
      [internalId],
    )
    const row = rows[0]
    if (!row) {
      return null
    }
    const stored: StoredEncryptedDocument = {
      keyId: row.key_id,
      wrappedDek: row.wrapped_dek,
      nonce: row.nonce,
      authTag: row.auth_tag,
      ciphertext: row.ciphertext,
    }
    const plaintext = this.#cipher.decrypt(stored)
    if (
      plaintext.byteLength !== Number(row.plaintext_size) ||
      !sha256(plaintext).equals(row.plaintext_sha256)
    ) {
      throw new Error('Encrypted document integrity metadata does not match its plaintext')
    }
    return plaintext
  }
}
