CREATE TABLE certificates (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    display_name VARCHAR(255) NOT NULL,
    lifecycle_state VARCHAR(32) NOT NULL DEFAULT 'active',
    fingerprint_sha256 BINARY(32) NOT NULL,
    serial_number VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    subject VARCHAR(2048) NOT NULL,
    issuer VARCHAR(2048) NOT NULL,
    dns_names_json JSON NOT NULL,
    not_before DATETIME(6) NOT NULL,
    not_after DATETIME(6) NOT NULL,
    key_type VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    certificate_document_id BIGINT UNSIGNED NOT NULL,
    private_key_document_id BIGINT UNSIGNED NOT NULL,
    created_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    superseded_at DATETIME(6) NULL,
    UNIQUE KEY uk_certificates_public_id (public_id),
    UNIQUE KEY uk_certificates_environment_fingerprint (environment_id, fingerprint_sha256),
    KEY ix_certificates_environment_expiry (environment_id, not_after, id),
    CONSTRAINT fk_certificates_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_certificates_certificate_document
        FOREIGN KEY (certificate_document_id) REFERENCES config_documents (id),
    CONSTRAINT fk_certificates_private_key_document
        FOREIGN KEY (private_key_document_id) REFERENCES config_documents (id),
    CONSTRAINT fk_certificates_created_by FOREIGN KEY (created_by) REFERENCES users (id),
    CONSTRAINT ck_certificates_validity CHECK (not_after > not_before)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE certificate_bindings (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    project_id BIGINT UNSIGNED NOT NULL,
    certificate_id BIGINT UNSIGNED NOT NULL,
    bound_by BIGINT UNSIGNED NOT NULL,
    bound_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    unbound_by BIGINT UNSIGNED NULL,
    unbound_at DATETIME(6) NULL,
    active_project_id BIGINT UNSIGNED
        GENERATED ALWAYS AS (IF(unbound_at IS NULL, project_id, NULL)) STORED,
    UNIQUE KEY uk_certificate_bindings_public_id (public_id),
    UNIQUE KEY uk_certificate_bindings_active_project (active_project_id),
    KEY ix_certificate_bindings_certificate (certificate_id, unbound_at, id),
    KEY ix_certificate_bindings_project_history (project_id, bound_at, id),
    CONSTRAINT fk_certificate_bindings_project FOREIGN KEY (project_id) REFERENCES projects (id),
    CONSTRAINT fk_certificate_bindings_certificate
        FOREIGN KEY (certificate_id) REFERENCES certificates (id),
    CONSTRAINT fk_certificate_bindings_bound_by FOREIGN KEY (bound_by) REFERENCES users (id),
    CONSTRAINT fk_certificate_bindings_unbound_by FOREIGN KEY (unbound_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
