#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
environment_file="$repository_root/.env"

if [ -e "$environment_file" ]; then
    echo "Keeping existing local environment file: $environment_file"
    exit 0
fi

if ! command -v openssl >/dev/null 2>&1; then
    echo "openssl is required to generate local demo secrets" >&2
    exit 1
fi

umask 077
mysql_root_password=$(openssl rand -hex 24)
mysql_password=$(openssl rand -hex 24)
document_key=$(openssl rand -base64 32)
rnacos_password=$(openssl rand -hex 24)

{
    echo "MYSQL_ROOT_PASSWORD=$mysql_root_password"
    echo "MYSQL_PASSWORD=$mysql_password"
    echo "DOCUMENT_ENCRYPTION_KEY_BASE64=$document_key"
    echo "RNACOS_ADMIN_USERNAME=admin"
    echo "RNACOS_ADMIN_PASSWORD=$rnacos_password"
    echo "CONSOLE_HTTP_PORT=8088"
    echo "MYSQL_PUBLISHED_PORT=3307"
    echo "RNACOS_HTTP_PORT=8848"
    echo "RNACOS_GRPC_PORT=9848"
    echo "RNACOS_CONSOLE_PORT=10848"
    echo "ACCESS_SERVER_HTTP_PORT=16688"
    echo "ACCESS_SERVER_METRICS_PORT=16689"
    echo "NATIVE_BUILD_JOBS=2"
} > "$environment_file"

echo "Created local demo environment: $environment_file"
