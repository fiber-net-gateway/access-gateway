ALTER TABLE draft_revisions
    ADD COLUMN restored_from_revision_id BIGINT UNSIGNED NULL AFTER parent_revision_id,
    ADD COLUMN route_count INT UNSIGNED NOT NULL DEFAULT 0 AFTER validation_state,
    ADD COLUMN idempotency_key VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NULL AFTER change_summary,
    ADD COLUMN request_sha256 BINARY(32) NULL AFTER idempotency_key,
    ADD KEY ix_draft_revisions_history (draft_id, revision_no DESC),
    ADD UNIQUE KEY uk_draft_revisions_idempotency (draft_id, created_by, idempotency_key),
    ADD CONSTRAINT fk_draft_revisions_restored_from
        FOREIGN KEY (restored_from_revision_id) REFERENCES draft_revisions (id);

ALTER TABLE validation_runs
    ADD COLUMN compiler_revision VARCHAR(64) NULL AFTER status;

ALTER TABLE releases
    ADD COLUMN compiler_revision VARCHAR(64) NULL AFTER rollback_of_release_id,
    ADD COLUMN current_revision_id_at_creation BIGINT UNSIGNED NULL AFTER compiler_revision,
    ADD COLUMN idempotency_key VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NULL AFTER current_revision_id_at_creation,
    ADD COLUMN request_sha256 BINARY(32) NULL AFTER idempotency_key,
    ADD COLUMN validation_errors_json JSON NULL AFTER request_sha256,
    ADD UNIQUE KEY uk_releases_idempotency (environment_id, created_by, idempotency_key),
    ADD CONSTRAINT fk_releases_current_revision
        FOREIGN KEY (current_revision_id_at_creation) REFERENCES draft_revisions (id);

ALTER TABLE release_items
    ADD COLUMN source_relation VARCHAR(32) NULL AFTER draft_revision_id,
    ADD KEY ix_release_items_project_version (project_id, draft_revision_id, release_id);
