ALTER TABLE releases
    MODIFY COLUMN native_validator_contract INT UNSIGNED NULL,
    MODIFY COLUMN native_validator_revision VARCHAR(64) NULL;
