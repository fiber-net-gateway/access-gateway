#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
environment_file="$repository_root/.env"
certificate_directory="$repository_root/deploy/demo/certs"
certificate_file="$certificate_directory/demo.crt"
private_key_file="$certificate_directory/demo.key"

if ! command -v openssl >/dev/null 2>&1; then
    echo "openssl is required to generate local demo secrets" >&2
    exit 1
fi

umask 077
if [ -e "$environment_file" ]; then
    echo "Keeping existing local environment file: $environment_file"
    if ! grep -q '^ACCESS_SERVER_ACTIVATION_TOKEN=' "$environment_file"; then
        activation_token=$(openssl rand -hex 32)
        echo "ACCESS_SERVER_ACTIVATION_TOKEN=$activation_token" >> "$environment_file"
        echo "Added the access-server activation token to the local environment file"
    fi
else
    mysql_root_password=$(openssl rand -hex 24)
    mysql_password=$(openssl rand -hex 24)
    document_key=$(openssl rand -base64 32)
    rnacos_password=$(openssl rand -hex 24)
    activation_token=$(openssl rand -hex 32)

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
        echo "ACCESS_SERVER_PUBLISHED_HOST=0.0.0.0"
        echo "ACCESS_SERVER_HTTPS_PORT=16688"
        echo "ACCESS_SERVER_METRICS_PORT=16689"
        echo "ACCESS_SERVER_ACTIVATION_TOKEN=$activation_token"
        echo "NATIVE_BUILD_JOBS=2"
    } > "$environment_file"

    echo "Created local demo environment: $environment_file"
fi

if [ -e "$certificate_file" ] && [ -e "$private_key_file" ]; then
    echo "Keeping existing local demo certificate: $certificate_file"
    exit 0
fi

mkdir -p "$certificate_directory"
openssl req -x509 -new -newkey ec \
    -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes \
    -keyout "$private_key_file" \
    -out "$certificate_file" \
    -sha256 \
    -days 825 \
    -subj "/CN=demo.local" \
    -addext "subjectAltName=DNS:demo.local,DNS:localhost,IP:127.0.0.1,IP:::1" \
    >/dev/null 2>&1
chmod 0600 "$private_key_file" "$certificate_file"
echo "Created local demo certificate: $certificate_file"
