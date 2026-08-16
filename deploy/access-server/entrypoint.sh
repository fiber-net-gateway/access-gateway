#!/bin/sh
set -eu

rnacos_host="${RNACOS_HOST:-rnacos}"
activation_token="${ACCESS_SERVER_ACTIVATION_TOKEN:-}"
case "$activation_token" in
    *[!0-9a-f]*|'')
        echo "ACCESS_SERVER_ACTIVATION_TOKEN must be lowercase hexadecimal" >&2
        exit 1
        ;;
esac
if [ "${#activation_token}" -ne 64 ]; then
    echo "ACCESS_SERVER_ACTIVATION_TOKEN must contain 64 hexadecimal characters" >&2
    exit 1
fi
set -- $(getent ahostsv4 "$rnacos_host")
if [ "$#" -eq 0 ] || [ -z "${1:-}" ]; then
    echo "cannot resolve R-Nacos host: $rnacos_host" >&2
    exit 1
fi
rnacos_ip="$1"

umask 077
sed -e "s/__RNACOS_IP__/${rnacos_ip}/g" \
    -e "s/__ACTIVATION_TOKEN__/${activation_token}/g" \
    /etc/access-gateway/access-server.demo.conf \
    > /tmp/access-server.env

exec /usr/local/bin/access-server /tmp/access-server.env
