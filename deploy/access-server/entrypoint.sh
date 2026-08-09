#!/bin/sh
set -eu

rnacos_host="${RNACOS_HOST:-rnacos}"
set -- $(getent ahostsv4 "$rnacos_host")
if [ "$#" -eq 0 ] || [ -z "${1:-}" ]; then
    echo "cannot resolve R-Nacos host: $rnacos_host" >&2
    exit 1
fi
rnacos_ip="$1"

sed "s/__RNACOS_IP__/${rnacos_ip}/g" \
    /etc/access-gateway/access-server.demo.conf \
    > /tmp/access-server.env

exec /usr/local/bin/access-server /tmp/access-server.env
