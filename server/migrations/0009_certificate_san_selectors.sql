CREATE TEMPORARY TABLE certificate_san_selector_preflight (
    environment_id BIGINT UNSIGNED NOT NULL,
    dns_name VARCHAR(253) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    UNIQUE KEY uk_certificate_san_selector_preflight (environment_id, dns_name)
) ENGINE = InnoDB;

INSERT INTO certificate_san_selector_preflight (environment_id, dns_name)
SELECT series_record.environment_id, names_record.dns_name
FROM certificate_series series_record
INNER JOIN certificates current_version
    ON current_version.id = series_record.current_version_id
INNER JOIN JSON_TABLE(
    current_version.dns_names_json,
    '$[*]' COLUMNS (dns_name VARCHAR(253) PATH '$')
) AS names_record
WHERE series_record.archived_at IS NULL;

DROP TEMPORARY TABLE certificate_san_selector_preflight;

CREATE TABLE certificate_san_selectors (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    environment_id BIGINT UNSIGNED NOT NULL,
    certificate_series_id BIGINT UNSIGNED NOT NULL,
    dns_name VARCHAR(253) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    match_kind VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    match_value VARCHAR(253) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    wildcard_dot_count TINYINT UNSIGNED NULL,
    UNIQUE KEY uk_certificate_san_selectors_environment_name (environment_id, dns_name),
    KEY ix_certificate_san_selectors_series (certificate_series_id, dns_name),
    KEY ix_certificate_san_selectors_match
        (environment_id, match_kind, match_value, wildcard_dot_count),
    CONSTRAINT fk_certificate_san_selectors_environment
        FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_certificate_san_selectors_series
        FOREIGN KEY (certificate_series_id) REFERENCES certificate_series (id),
    CONSTRAINT ck_certificate_san_selectors_kind
        CHECK (match_kind IN ('exact', 'wildcard')),
    CONSTRAINT ck_certificate_san_selectors_wildcard
        CHECK (
            (match_kind = 'exact' AND wildcard_dot_count IS NULL) OR
            (match_kind = 'wildcard' AND wildcard_dot_count IS NOT NULL)
        )
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

INSERT INTO certificate_san_selectors
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
INNER JOIN certificates current_version
    ON current_version.id = series_record.current_version_id
INNER JOIN JSON_TABLE(
    current_version.dns_names_json,
    '$[*]' COLUMNS (dns_name VARCHAR(253) PATH '$')
) AS names_record
WHERE series_record.archived_at IS NULL;
