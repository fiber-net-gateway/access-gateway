CREATE TABLE users (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    subject VARCHAR(255) NOT NULL,
    display_name VARCHAR(255) NOT NULL,
    email VARCHAR(320) NULL,
    status VARCHAR(32) NOT NULL,
    is_platform_admin BOOLEAN NOT NULL DEFAULT FALSE,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_users_public_id (public_id),
    UNIQUE KEY uk_users_subject (subject)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE user_sessions (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    user_id BIGINT UNSIGNED NOT NULL,
    token_sha256 BINARY(32) NOT NULL,
    csrf_sha256 BINARY(32) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    last_seen_at DATETIME(6) NOT NULL,
    revoked_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_user_sessions_public_id (public_id),
    UNIQUE KEY uk_user_sessions_token (token_sha256),
    KEY ix_user_sessions_user_expiry (user_id, expires_at),
    CONSTRAINT fk_user_sessions_user FOREIGN KEY (user_id) REFERENCES users (id)
) ENGINE = InnoDB;

CREATE TABLE secret_references (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    provider VARCHAR(32) NOT NULL,
    locator VARCHAR(1024) NOT NULL,
    display_name VARCHAR(255) NOT NULL,
    metadata_json JSON NULL,
    created_by BIGINT UNSIGNED NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    rotated_at DATETIME(6) NULL,
    UNIQUE KEY uk_secret_references_public_id (public_id),
    CONSTRAINT fk_secret_references_created_by FOREIGN KEY (created_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE environments (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    code VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    name VARCHAR(255) NOT NULL,
    tier VARCHAR(32) NOT NULL,
    status VARCHAR(32) NOT NULL,
    nacos_endpoint VARCHAR(2048) NOT NULL,
    nacos_namespace VARCHAR(255) NOT NULL,
    nacos_tenant VARCHAR(255) NOT NULL,
    nacos_secret_ref_id BIGINT UNSIGNED NULL,
    projects_data_id VARCHAR(512) NOT NULL,
    route_data_id_prefix VARCHAR(512) NOT NULL,
    route_group VARCHAR(255) NOT NULL,
    gray_data_id VARCHAR(512) NOT NULL,
    gray_group VARCHAR(255) NOT NULL,
    naming_group VARCHAR(255) NOT NULL,
    zone VARCHAR(255) NOT NULL,
    protection_policy JSON NOT NULL,
    last_release_sequence BIGINT UNSIGNED NOT NULL DEFAULT 0,
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_by BIGINT UNSIGNED NOT NULL,
    updated_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_environments_public_id (public_id),
    UNIQUE KEY uk_environments_code (code),
    CONSTRAINT fk_environments_nacos_secret
        FOREIGN KEY (nacos_secret_ref_id) REFERENCES secret_references (id),
    CONSTRAINT fk_environments_created_by FOREIGN KEY (created_by) REFERENCES users (id),
    CONSTRAINT fk_environments_updated_by FOREIGN KEY (updated_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE environment_memberships (
    environment_id BIGINT UNSIGNED NOT NULL,
    user_id BIGINT UNSIGNED NOT NULL,
    role VARCHAR(32) NOT NULL,
    created_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (environment_id, user_id),
    KEY ix_environment_memberships_user (user_id, environment_id),
    CONSTRAINT fk_environment_memberships_environment
        FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_environment_memberships_user FOREIGN KEY (user_id) REFERENCES users (id),
    CONSTRAINT fk_environment_memberships_created_by FOREIGN KEY (created_by) REFERENCES users (id)
) ENGINE = InnoDB;

CREATE TABLE config_documents (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    purpose VARCHAR(32) NOT NULL,
    content_type VARCHAR(128) NOT NULL,
    schema_version INT UNSIGNED NULL,
    plaintext_sha256 BINARY(32) NOT NULL,
    plaintext_size BIGINT UNSIGNED NOT NULL,
    key_id VARCHAR(255) NOT NULL,
    wrapped_dek VARBINARY(1024) NOT NULL,
    nonce BINARY(12) NOT NULL,
    auth_tag BINARY(16) NOT NULL,
    ciphertext LONGBLOB NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_config_documents_public_id (public_id),
    KEY ix_config_documents_digest (environment_id, plaintext_sha256),
    CONSTRAINT fk_config_documents_environment FOREIGN KEY (environment_id) REFERENCES environments (id)
) ENGINE = InnoDB;

CREATE TABLE audit_events (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NULL,
    actor_user_id BIGINT UNSIGNED NULL,
    event_type VARCHAR(64) NOT NULL,
    target_type VARCHAR(64) NOT NULL,
    target_public_id BINARY(16) NULL,
    request_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    result VARCHAR(32) NOT NULL,
    summary_json JSON NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_audit_events_public_id (public_id),
    KEY ix_audit_events_environment_time (environment_id, created_at, id),
    KEY ix_audit_events_actor_time (actor_user_id, created_at, id),
    CONSTRAINT fk_audit_events_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_audit_events_actor FOREIGN KEY (actor_user_id) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE outbox_events (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NULL,
    topic VARCHAR(64) NOT NULL,
    dedupe_key VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    payload_json JSON NOT NULL,
    state VARCHAR(32) NOT NULL,
    attempt_count INT UNSIGNED NOT NULL DEFAULT 0,
    next_run_at DATETIME(6) NOT NULL,
    lease_owner VARCHAR(128) NULL,
    lease_token BINARY(16) NULL,
    lease_expires_at DATETIME(6) NULL,
    last_error_code VARCHAR(64) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    delivered_at DATETIME(6) NULL,
    UNIQUE KEY uk_outbox_events_public_id (public_id),
    UNIQUE KEY uk_outbox_events_dedupe (topic, dedupe_key),
    KEY ix_outbox_events_claim (state, next_run_at, lease_expires_at, id),
    CONSTRAINT fk_outbox_events_environment FOREIGN KEY (environment_id) REFERENCES environments (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE api_idempotency_records (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    actor_user_id BIGINT UNSIGNED NOT NULL,
    environment_id BIGINT UNSIGNED NULL,
    operation VARCHAR(64) NOT NULL,
    idempotency_key VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    request_sha256 BINARY(32) NOT NULL,
    state VARCHAR(32) NOT NULL,
    response_status SMALLINT UNSIGNED NULL,
    response_document_id BIGINT UNSIGNED NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    expires_at DATETIME(6) NOT NULL,
    UNIQUE KEY uk_api_idempotency_scope (actor_user_id, environment_id, operation, idempotency_key),
    KEY ix_api_idempotency_expiry (expires_at),
    CONSTRAINT fk_api_idempotency_actor FOREIGN KEY (actor_user_id) REFERENCES users (id),
    CONSTRAINT fk_api_idempotency_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_api_idempotency_response FOREIGN KEY (response_document_id) REFERENCES config_documents (id)
) ENGINE = InnoDB;
