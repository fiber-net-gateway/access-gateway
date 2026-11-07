ALTER TABLE instance_observations
    ADD COLUMN edge_watcher_state VARCHAR(32) NULL AFTER discovery_selector_leases,
    ADD COLUMN edge_observed_md5 BINARY(16) NULL AFTER edge_watcher_state,
    ADD COLUMN edge_active_md5 BINARY(16) NULL AFTER edge_observed_md5,
    ADD COLUMN edge_candidate_status VARCHAR(32) NULL AFTER edge_active_md5,
    ADD COLUMN edge_error_code VARCHAR(64) NULL AFTER edge_candidate_status,
    ADD COLUMN edge_generation BIGINT UNSIGNED NULL AFTER edge_error_code,
    ADD COLUMN edge_function_count INT UNSIGNED NULL AFTER edge_generation,
    ADD COLUMN edge_total_script_bytes BIGINT UNSIGNED NULL AFTER edge_function_count,
    ADD COLUMN edge_active_index_sha256 BINARY(32) NULL AFTER edge_total_script_bytes;
