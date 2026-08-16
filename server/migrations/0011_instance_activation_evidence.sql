ALTER TABLE access_server_instances
    MODIFY created_by BIGINT UNSIGNED NULL,
    ADD COLUMN next_poll_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) AFTER updated_at,
    ADD COLUMN last_polled_at DATETIME(6) NULL AFTER next_poll_at,
    ADD COLUMN lease_owner VARCHAR(128) NULL AFTER last_seen_at,
    ADD COLUMN lease_token BINARY(16) NULL AFTER lease_owner,
    ADD COLUMN lease_expires_at DATETIME(6) NULL AFTER lease_token,
    DROP INDEX ix_access_server_instances_poll,
    ADD KEY ix_access_server_instances_poll (enabled, next_poll_at, lease_expires_at, id);

ALTER TABLE instance_observations
    CHANGE COLUMN watcher_state route_watcher_state VARCHAR(32) NULL,
    MODIFY build_revision VARCHAR(80) NULL,
    ADD COLUMN evidence_revision BIGINT UNSIGNED NULL AFTER contract_version,
    ADD COLUMN instance_started_at DATETIME(6) NULL AFTER runtime_state,
    ADD COLUMN route_readiness_state VARCHAR(32) NULL AFTER route_watcher_state,
    ADD COLUMN route_snapshot_generation BIGINT UNSIGNED NULL AFTER route_readiness_state,
    ADD COLUMN route_snapshot_fingerprint BINARY(32) NULL AFTER route_snapshot_generation,
    ADD COLUMN route_snapshot_published_at DATETIME(6) NULL AFTER route_snapshot_fingerprint,
    ADD COLUMN project_list_observed_md5 BINARY(16) NULL AFTER project_list_md5,
    ADD COLUMN project_list_candidate_status VARCHAR(32) NULL AFTER project_list_observed_md5,
    ADD COLUMN project_list_error_code VARCHAR(64) NULL AFTER project_list_candidate_status,
    ADD COLUMN project_list_observed_at DATETIME(6) NULL AFTER project_list_error_code,
    ADD COLUMN project_list_active_at DATETIME(6) NULL AFTER project_list_observed_at,
    ADD COLUMN gray_observed_md5 BINARY(16) NULL AFTER gray_md5,
    ADD COLUMN gray_candidate_status VARCHAR(32) NULL AFTER gray_observed_md5,
    ADD COLUMN gray_error_code VARCHAR(64) NULL AFTER gray_candidate_status,
    ADD COLUMN gray_generation BIGINT UNSIGNED NULL AFTER gray_error_code,
    ADD COLUMN tls_enabled BOOLEAN NULL AFTER gray_rule_count,
    ADD COLUMN tls_observed_md5 BINARY(16) NULL AFTER tls_enabled,
    ADD COLUMN tls_active_md5 BINARY(16) NULL AFTER tls_observed_md5,
    ADD COLUMN tls_candidate_status VARCHAR(32) NULL AFTER tls_active_md5,
    ADD COLUMN tls_error_code VARCHAR(64) NULL AFTER tls_candidate_status,
    ADD COLUMN tls_version BIGINT UNSIGNED NULL AFTER tls_error_code,
    ADD COLUMN tls_certificate_count INT UNSIGNED NULL AFTER tls_version,
    ADD COLUMN discovery_client_state VARCHAR(32) NULL AFTER tls_certificate_count,
    ADD COLUMN discovery_config_state VARCHAR(32) NULL AFTER discovery_client_state,
    ADD COLUMN discovery_naming_state VARCHAR(32) NULL AFTER discovery_config_state,
    ADD COLUMN discovery_ready_services INT UNSIGNED NULL AFTER discovery_naming_state,
    ADD COLUMN discovery_selectable_endpoints INT UNSIGNED NULL AFTER discovery_ready_services,
    ADD COLUMN discovery_logical_clusters INT UNSIGNED NULL AFTER discovery_selectable_endpoints,
    ADD COLUMN discovery_selector_leases INT UNSIGNED NULL AFTER discovery_logical_clusters;

ALTER TABLE instance_project_observations
    CHANGE COLUMN status candidate_status VARCHAR(32) NOT NULL,
    ADD COLUMN subscription_state VARCHAR(32) NOT NULL DEFAULT 'unknown' AFTER project_name,
    ADD COLUMN observed_project_version INT NULL AFTER subscription_state,
    ADD COLUMN observed_route_md5 BINARY(16) NULL AFTER observed_project_version,
    ADD COLUMN active_snapshot_generation BIGINT UNSIGNED NULL AFTER route_md5,
    ADD COLUMN active_loaded BOOLEAN NOT NULL DEFAULT FALSE AFTER active_snapshot_generation,
    ADD COLUMN observed_at DATETIME(6) NULL AFTER active_loaded,
    ADD COLUMN active_at DATETIME(6) NULL AFTER observed_at;

ALTER TABLE release_activation_targets
    ADD KEY ix_release_activation_targets_instance (instance_id, required_target, release_id);

ALTER TABLE release_instance_activations
    ADD KEY ix_release_instance_activations_expiry (expires_at, release_id, instance_id);
