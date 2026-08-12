CREATE TABLE certificate_series (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    display_name VARCHAR(255) NOT NULL,
    managed_dns_names_json JSON NOT NULL,
    current_version_id BIGINT UNSIGNED NULL,
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    archived_at DATETIME(6) NULL,
    UNIQUE KEY uk_certificate_series_public_id (public_id),
    KEY ix_certificate_series_environment_name (environment_id, display_name, id),
    CONSTRAINT fk_certificate_series_environment
        FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_certificate_series_current_version
        FOREIGN KEY (current_version_id) REFERENCES certificates (id),
    CONSTRAINT fk_certificate_series_created_by FOREIGN KEY (created_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

INSERT INTO certificate_series
    (public_id, environment_id, display_name, managed_dns_names_json,
     current_version_id, created_by, created_at, updated_at)
SELECT
    public_id, environment_id, display_name, dns_names_json,
    id, created_by, created_at, created_at
FROM certificates;

ALTER TABLE certificates
    ADD COLUMN series_id BIGINT UNSIGNED NULL AFTER environment_id,
    ADD COLUMN version_no INT UNSIGNED NULL AFTER series_id;

UPDATE certificates certificate_version
INNER JOIN certificate_series series_record
    ON series_record.current_version_id = certificate_version.id
SET certificate_version.series_id = series_record.id,
    certificate_version.version_no = 1;

ALTER TABLE certificates
    MODIFY COLUMN series_id BIGINT UNSIGNED NOT NULL,
    MODIFY COLUMN version_no INT UNSIGNED NOT NULL,
    ADD UNIQUE KEY uk_certificates_series_version (series_id, version_no),
    ADD KEY ix_certificates_series_created (series_id, created_at, id),
    ADD CONSTRAINT fk_certificates_series
        FOREIGN KEY (series_id) REFERENCES certificate_series (id);

CREATE TABLE certificate_dns_names (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    environment_id BIGINT UNSIGNED NOT NULL,
    certificate_series_id BIGINT UNSIGNED NOT NULL,
    dns_name VARCHAR(253) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    match_kind VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    match_value VARCHAR(253) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    wildcard_dot_count TINYINT UNSIGNED NULL,
    KEY ix_certificate_dns_names_environment_name (environment_id, dns_name),
    KEY ix_certificate_dns_names_series (certificate_series_id, dns_name),
    KEY ix_certificate_dns_names_match
        (environment_id, match_kind, match_value, wildcard_dot_count),
    CONSTRAINT fk_certificate_dns_names_environment
        FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_certificate_dns_names_series
        FOREIGN KEY (certificate_series_id) REFERENCES certificate_series (id),
    CONSTRAINT ck_certificate_dns_names_kind
        CHECK (match_kind IN ('exact', 'wildcard')),
    CONSTRAINT ck_certificate_dns_names_wildcard
        CHECK (
            (match_kind = 'exact' AND wildcard_dot_count IS NULL) OR
            (match_kind = 'wildcard' AND wildcard_dot_count IS NOT NULL)
        )
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

INSERT INTO certificate_dns_names
    (environment_id, certificate_series_id, dns_name, match_kind, match_value,
     wildcard_dot_count)
SELECT
    series_record.environment_id,
    series_record.id,
    names_record.dns_name,
    IF(names_record.dns_name LIKE '*.%', 'wildcard', 'exact'),
    IF(names_record.dns_name LIKE '*.%', SUBSTRING(names_record.dns_name, 3), names_record.dns_name),
    IF(
        names_record.dns_name LIKE '*.%',
        LENGTH(names_record.dns_name) - LENGTH(REPLACE(names_record.dns_name, '.', '')),
        NULL
    )
FROM certificate_series series_record
INNER JOIN JSON_TABLE(
    series_record.managed_dns_names_json,
    '$[*]' COLUMNS (dns_name VARCHAR(253) PATH '$')
) AS names_record;
