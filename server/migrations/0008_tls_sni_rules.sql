ALTER TABLE certificate_dns_names
    ADD COLUMN public_id BINARY(16) NULL AFTER id,
    ADD COLUMN lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER wildcard_dot_count,
    ADD COLUMN source VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin
        NOT NULL DEFAULT 'legacy_san_import' AFTER lock_version,
    ADD COLUMN created_by BIGINT UNSIGNED NULL AFTER source,
    ADD COLUMN created_at DATETIME(6) NULL AFTER created_by,
    ADD COLUMN updated_at DATETIME(6) NULL AFTER created_at,
    ADD COLUMN archived_at DATETIME(6) NULL AFTER updated_at;

UPDATE certificate_dns_names rule_record
INNER JOIN certificate_series series_record
    ON series_record.id = rule_record.certificate_series_id
SET rule_record.public_id = UUID_TO_BIN(UUID()),
    rule_record.created_by = series_record.created_by,
    rule_record.created_at = series_record.created_at,
    rule_record.updated_at = series_record.updated_at;

ALTER TABLE certificate_dns_names
    MODIFY COLUMN public_id BINARY(16) NOT NULL,
    MODIFY COLUMN created_by BIGINT UNSIGNED NOT NULL,
    MODIFY COLUMN created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    MODIFY COLUMN updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    ADD UNIQUE KEY uk_certificate_dns_names_public_id (public_id),
    ADD KEY ix_certificate_dns_names_active
        (environment_id, archived_at, match_kind, match_value, wildcard_dot_count),
    ADD CONSTRAINT fk_certificate_dns_names_created_by
        FOREIGN KEY (created_by) REFERENCES users (id),
    ADD CONSTRAINT ck_certificate_dns_names_source
        CHECK (source IN ('explicit', 'legacy_san_import'));
