CREATE TABLE access_server_instances (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    instance_key VARCHAR(255) NOT NULL,
    source VARCHAR(32) NOT NULL,
    status_endpoint VARCHAR(2048) NOT NULL,
    status_secret_ref_id BIGINT UNSIGNED NULL,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    poll_interval_millis INT UNSIGNED NOT NULL,
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    last_seen_at DATETIME(6) NULL,
    UNIQUE KEY uk_access_server_instances_public_id (public_id),
    UNIQUE KEY uk_access_server_instances_key (environment_id, instance_key),
    KEY ix_access_server_instances_poll (enabled, environment_id, last_seen_at, id),
    CONSTRAINT fk_access_server_instances_environment
        FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_access_server_instances_secret
        FOREIGN KEY (status_secret_ref_id) REFERENCES secret_references (id),
    CONSTRAINT fk_access_server_instances_created_by FOREIGN KEY (created_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE instance_observations (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    instance_id BIGINT UNSIGNED NOT NULL,
    contract_version INT UNSIGNED NULL,
    poll_result VARCHAR(32) NOT NULL,
    build_version VARCHAR(128) NULL,
    build_revision VARCHAR(64) NULL,
    runtime_state VARCHAR(32) NULL,
    watcher_state VARCHAR(32) NULL,
    project_list_md5 BINARY(16) NULL,
    project_list_ready BOOLEAN NULL,
    gray_md5 BINARY(16) NULL,
    gray_rule_count INT UNSIGNED NULL,
    error_code VARCHAR(64) NULL,
    observed_at DATETIME(6) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    UNIQUE KEY uk_instance_observations_public_id (public_id),
    KEY ix_instance_observations_latest (instance_id, observed_at, id),
    CONSTRAINT fk_instance_observations_instance FOREIGN KEY (instance_id) REFERENCES access_server_instances (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE instance_project_observations (
    instance_observation_id BIGINT UNSIGNED NOT NULL,
    project_name VARCHAR(255) NOT NULL,
    project_version INT NULL,
    route_md5 BINARY(16) NULL,
    status VARCHAR(32) NOT NULL,
    error_code VARCHAR(64) NULL,
    error_field VARCHAR(1024) NULL,
    error_offset BIGINT UNSIGNED NULL,
    error_message VARCHAR(2048) NULL,
    PRIMARY KEY (instance_observation_id, project_name),
    CONSTRAINT fk_instance_project_observations_observation
        FOREIGN KEY (instance_observation_id) REFERENCES instance_observations (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE TABLE release_activation_targets (
    release_id BIGINT UNSIGNED NOT NULL,
    instance_id BIGINT UNSIGNED NOT NULL,
    required_target BOOLEAN NOT NULL DEFAULT TRUE,
    PRIMARY KEY (release_id, instance_id),
    CONSTRAINT fk_release_activation_targets_release FOREIGN KEY (release_id) REFERENCES releases (id),
    CONSTRAINT fk_release_activation_targets_instance
        FOREIGN KEY (instance_id) REFERENCES access_server_instances (id)
) ENGINE = InnoDB;

CREATE TABLE release_instance_activations (
    release_id BIGINT UNSIGNED NOT NULL,
    instance_id BIGINT UNSIGNED NOT NULL,
    status VARCHAR(32) NOT NULL,
    supporting_observation_id BIGINT UNSIGNED NULL,
    evaluated_at DATETIME(6) NOT NULL,
    expires_at DATETIME(6) NULL,
    PRIMARY KEY (release_id, instance_id),
    KEY ix_release_instance_activations_status (release_id, status),
    CONSTRAINT fk_release_instance_activations_release FOREIGN KEY (release_id) REFERENCES releases (id),
    CONSTRAINT fk_release_instance_activations_instance
        FOREIGN KEY (instance_id) REFERENCES access_server_instances (id),
    CONSTRAINT fk_release_instance_activations_observation
        FOREIGN KEY (supporting_observation_id) REFERENCES instance_observations (id)
) ENGINE = InnoDB;
