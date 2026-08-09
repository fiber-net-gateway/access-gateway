CREATE TABLE projects (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    name VARCHAR(255) NOT NULL,
    status VARCHAR(32) NOT NULL,
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    archived_at DATETIME(6) NULL,
    UNIQUE KEY uk_projects_public_id (public_id),
    UNIQUE KEY uk_projects_environment_name (environment_id, name),
    KEY ix_projects_environment_status (environment_id, status, name),
    CONSTRAINT fk_projects_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_projects_created_by FOREIGN KEY (created_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE project_version_counters (
    project_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    last_allocated_version INT NOT NULL DEFAULT 0,
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    CONSTRAINT fk_project_version_counters_project FOREIGN KEY (project_id) REFERENCES projects (id),
    CONSTRAINT ck_project_version_nonnegative CHECK (last_allocated_version >= 0)
) ENGINE = InnoDB;

CREATE TABLE nacos_resource_observations (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    data_id VARCHAR(512) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    group_name VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    resource_exists BOOLEAN NOT NULL,
    payload_document_id BIGINT UNSIGNED NULL,
    nacos_md5 BINARY(16) NULL,
    sha256 BINARY(32) NULL,
    source VARCHAR(32) NOT NULL,
    client_result VARCHAR(32) NOT NULL,
    error_code VARCHAR(64) NULL,
    fetched_at DATETIME(6) NOT NULL,
    UNIQUE KEY uk_nacos_resource_observations_public_id (public_id),
    KEY ix_nacos_observations_latest (environment_id, data_id, group_name, fetched_at, id),
    CONSTRAINT fk_nacos_observations_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_nacos_observations_payload FOREIGN KEY (payload_document_id) REFERENCES config_documents (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE drafts (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    project_id BIGINT UNSIGNED NULL,
    scope_key VARCHAR(320) NOT NULL,
    kind VARCHAR(32) NOT NULL,
    state VARCHAR(32) NOT NULL,
    title VARCHAR(255) NOT NULL,
    current_revision_no INT UNSIGNED NOT NULL DEFAULT 0,
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_by BIGINT UNSIGNED NOT NULL,
    updated_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    archived_at DATETIME(6) NULL,
    UNIQUE KEY uk_drafts_public_id (public_id),
    UNIQUE KEY uk_drafts_environment_scope (environment_id, scope_key),
    KEY ix_drafts_environment_state (environment_id, state, updated_at),
    CONSTRAINT fk_drafts_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_drafts_project FOREIGN KEY (project_id) REFERENCES projects (id),
    CONSTRAINT fk_drafts_created_by FOREIGN KEY (created_by) REFERENCES users (id),
    CONSTRAINT fk_drafts_updated_by FOREIGN KEY (updated_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE draft_revisions (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    draft_id BIGINT UNSIGNED NOT NULL,
    revision_no INT UNSIGNED NOT NULL,
    parent_revision_id BIGINT UNSIGNED NULL,
    model_document_id BIGINT UNSIGNED NOT NULL,
    source_document_id BIGINT UNSIGNED NULL,
    base_nacos_observation_id BIGINT UNSIGNED NULL,
    validation_state VARCHAR(32) NOT NULL,
    change_summary VARCHAR(1024) NOT NULL,
    created_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_draft_revisions_public_id (public_id),
    UNIQUE KEY uk_draft_revisions_number (draft_id, revision_no),
    CONSTRAINT fk_draft_revisions_draft FOREIGN KEY (draft_id) REFERENCES drafts (id),
    CONSTRAINT fk_draft_revisions_parent FOREIGN KEY (parent_revision_id) REFERENCES draft_revisions (id),
    CONSTRAINT fk_draft_revisions_model FOREIGN KEY (model_document_id) REFERENCES config_documents (id),
    CONSTRAINT fk_draft_revisions_source FOREIGN KEY (source_document_id) REFERENCES config_documents (id),
    CONSTRAINT fk_draft_revisions_base_observation
        FOREIGN KEY (base_nacos_observation_id) REFERENCES nacos_resource_observations (id),
    CONSTRAINT fk_draft_revisions_created_by FOREIGN KEY (created_by) REFERENCES users (id)
) ENGINE = InnoDB;

CREATE TABLE validation_runs (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    draft_revision_id BIGINT UNSIGNED NOT NULL,
    model_sha256 BINARY(32) NOT NULL,
    stage VARCHAR(32) NOT NULL,
    status VARCHAR(32) NOT NULL,
    validator_contract_version INT UNSIGNED NULL,
    validator_revision VARCHAR(64) NULL,
    errors_json JSON NOT NULL,
    started_at DATETIME(6) NOT NULL,
    finished_at DATETIME(6) NULL,
    UNIQUE KEY uk_validation_runs_public_id (public_id),
    KEY ix_validation_runs_revision (draft_revision_id, started_at, id),
    CONSTRAINT fk_validation_runs_revision FOREIGN KEY (draft_revision_id) REFERENCES draft_revisions (id)
) ENGINE = InnoDB;
