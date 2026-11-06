CREATE TABLE waf_policy_drafts (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    public_id BINARY(16) NOT NULL,
    environment_id BIGINT UNSIGNED NOT NULL,
    state VARCHAR(32) NOT NULL DEFAULT 'editing',
    lock_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
    policy_document_id BIGINT UNSIGNED NOT NULL,
    created_by BIGINT UNSIGNED NOT NULL,
    updated_by BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_waf_policy_drafts_public_id (public_id),
    UNIQUE KEY uk_waf_policy_drafts_environment (environment_id),
    CONSTRAINT fk_waf_policy_drafts_environment FOREIGN KEY (environment_id) REFERENCES environments (id),
    CONSTRAINT fk_waf_policy_drafts_policy_document FOREIGN KEY (policy_document_id) REFERENCES config_documents (id),
    CONSTRAINT fk_waf_policy_drafts_created_by FOREIGN KEY (created_by) REFERENCES users (id),
    CONSTRAINT fk_waf_policy_drafts_updated_by FOREIGN KEY (updated_by) REFERENCES users (id)
) ENGINE = InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
