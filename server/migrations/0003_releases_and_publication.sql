CREATE TABLE releases (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    sequence_no BIGINT UNSIGNED NOT NULL,
    kind VARCHAR(32) NOT NULL,
    status VARCHAR(32) NOT NULL,
    title VARCHAR(255) NOT NULL,
    description TEXT NOT NULL,
    rollback_of_release_id BIGINT UNSIGNED NULL,
    native_validator_contract INT UNSIGNED NOT NULL,
    native_validator_revision VARCHAR(64) NOT NULL,
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    ready_at DATETIME(6) NULL,
    publish_started_at DATETIME(6) NULL,
    published_at DATETIME(6) NULL,
    UNIQUE KEY uk_releases_public_id (public_id),
    UNIQUE KEY uk_releases_sequence (environment_id, sequence_no),
    KEY ix_releases_environment_status (environment_id, status, created_at),
    CONSTRAINT fk_releases_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_releases_rollback FOREIGN KEY (rollback_of_release_id) REFERENCES releases (id),
    CONSTRAINT fk_releases_created_by FOREIGN KEY (created_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE release_items (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    release_id BIGINT UNSIGNED NOT NULL,
    project_id BIGINT UNSIGNED NULL,
    kind VARCHAR(32) NOT NULL,
    draft_revision_id BIGINT UNSIGNED NULL,
    model_document_id BIGINT UNSIGNED NOT NULL,
    allocated_project_version INT NULL,
    change_kind VARCHAR(32) NOT NULL,
    diff_summary_json JSON NOT NULL,
    UNIQUE KEY uk_release_items_public_id (public_id),
    UNIQUE KEY uk_release_items_scope (release_id, kind, project_id),
    CONSTRAINT fk_release_items_release FOREIGN KEY (release_id) REFERENCES releases (id),
    CONSTRAINT fk_release_items_project FOREIGN KEY (project_id) REFERENCES projects (id),
    CONSTRAINT fk_release_items_revision FOREIGN KEY (draft_revision_id) REFERENCES draft_revisions (id),
    CONSTRAINT fk_release_items_model FOREIGN KEY (model_document_id) REFERENCES config_documents (id)
) ENGINE = InnoDB;

CREATE TABLE release_resources (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    release_id BIGINT UNSIGNED NOT NULL,
    project_id BIGINT UNSIGNED NULL,
    kind VARCHAR(32) NOT NULL,
    data_id VARCHAR(512) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    group_name VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    operation VARCHAR(16) NOT NULL,
    publish_order INT UNSIGNED NOT NULL,
    required_resource BOOLEAN NOT NULL DEFAULT TRUE,
    payload_document_id BIGINT UNSIGNED NULL,
    base_observation_id BIGINT UNSIGNED NULL,
    target_sha256 BINARY(32) NULL,
    allocated_project_version INT NULL,
    status VARCHAR(32) NOT NULL,
    verified_nacos_md5 BINARY(16) NULL,
    verified_sha256 BINARY(32) NULL,
    verified_at DATETIME(6) NULL,
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    UNIQUE KEY uk_release_resources_public_id (public_id),
    UNIQUE KEY uk_release_resources_data_id (release_id, data_id, group_name),
    KEY ix_release_resources_execution (release_id, status, publish_order),
    CONSTRAINT fk_release_resources_release FOREIGN KEY (release_id) REFERENCES releases (id),
    CONSTRAINT fk_release_resources_project FOREIGN KEY (project_id) REFERENCES projects (id),
    CONSTRAINT fk_release_resources_payload FOREIGN KEY (payload_document_id) REFERENCES config_documents (id),
    CONSTRAINT fk_release_resources_base_observation
        FOREIGN KEY (base_observation_id) REFERENCES nacos_resource_observations (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE release_resource_dependencies (
    resource_id BIGINT UNSIGNED NOT NULL,
    depends_on_resource_id BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (resource_id, depends_on_resource_id),
    CONSTRAINT fk_release_resource_dependencies_resource
        FOREIGN KEY (resource_id) REFERENCES release_resources (id),
    CONSTRAINT fk_release_resource_dependencies_parent
        FOREIGN KEY (depends_on_resource_id) REFERENCES release_resources (id),
    CONSTRAINT ck_release_resource_dependency_not_self CHECK (resource_id <> depends_on_resource_id)
) ENGINE = InnoDB;

CREATE TABLE release_approvals (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    release_id BIGINT UNSIGNED NOT NULL,
    actor_user_id BIGINT UNSIGNED NOT NULL,
    decision VARCHAR(32) NOT NULL,
    comment VARCHAR(1024) NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_release_approvals_public_id (public_id),
    UNIQUE KEY uk_release_approvals_actor (release_id, actor_user_id),
    CONSTRAINT fk_release_approvals_release FOREIGN KEY (release_id) REFERENCES releases (id),
    CONSTRAINT fk_release_approvals_actor FOREIGN KEY (actor_user_id) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE publication_jobs (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    release_id BIGINT UNSIGNED NOT NULL,
    state VARCHAR(32) NOT NULL,
    requested_by BIGINT UNSIGNED NOT NULL,
    requested_at DATETIME(6) NOT NULL,
    next_run_at DATETIME(6) NOT NULL,
    lease_owner VARCHAR(128) NULL,
    lease_token BINARY(16) NULL,
    lease_expires_at DATETIME(6) NULL,
    heartbeat_at DATETIME(6) NULL,
    cancel_requested BOOLEAN NOT NULL DEFAULT FALSE,
    attempt_count INT UNSIGNED NOT NULL DEFAULT 0,
    last_error_code VARCHAR(64) NULL,
    finished_at DATETIME(6) NULL,
    UNIQUE KEY uk_publication_jobs_public_id (public_id),
    UNIQUE KEY uk_publication_jobs_release (release_id),
    KEY ix_publication_jobs_claim (state, next_run_at, lease_expires_at, id),
    CONSTRAINT fk_publication_jobs_release FOREIGN KEY (release_id) REFERENCES releases (id),
    CONSTRAINT fk_publication_jobs_requested_by FOREIGN KEY (requested_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE publication_attempts (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    publication_job_id BIGINT UNSIGNED NOT NULL,
    release_resource_id BIGINT UNSIGNED NOT NULL,
    attempt_no INT UNSIGNED NOT NULL,
    idempotency_key VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    result VARCHAR(32) NOT NULL,
    before_exists BOOLEAN NULL,
    before_nacos_md5 BINARY(16) NULL,
    before_sha256 BINARY(32) NULL,
    after_nacos_md5 BINARY(16) NULL,
    after_sha256 BINARY(32) NULL,
    error_code VARCHAR(64) NULL,
    error_detail_json JSON NULL,
    started_at DATETIME(6) NOT NULL,
    finished_at DATETIME(6) NULL,
    UNIQUE KEY uk_publication_attempts_public_id (public_id),
    UNIQUE KEY uk_publication_attempts_number (release_resource_id, attempt_no),
    UNIQUE KEY uk_publication_attempts_idempotency (idempotency_key),
    CONSTRAINT fk_publication_attempts_job FOREIGN KEY (publication_job_id) REFERENCES publication_jobs (id),
    CONSTRAINT fk_publication_attempts_resource
        FOREIGN KEY (release_resource_id) REFERENCES release_resources (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE environment_publish_leases (
    environment_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    release_id BIGINT UNSIGNED NOT NULL,
    publication_job_id BIGINT UNSIGNED NOT NULL,
    lease_owner VARCHAR(128) NOT NULL,
    lease_token BINARY(16) NOT NULL,
    lease_expires_at DATETIME(6) NOT NULL,
    updated_at DATETIME(6) NOT NULL,
    CONSTRAINT fk_environment_publish_leases_environment
        FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_environment_publish_leases_release FOREIGN KEY (release_id) REFERENCES releases (id),
    CONSTRAINT fk_environment_publish_leases_job FOREIGN KEY (publication_job_id) REFERENCES publication_jobs (id)
) ENGINE = InnoDB;
