#!/usr/bin/env bash
# Applies repository-owned compatibility patches onto the pinned
# third_party/fiber-gateway-cpp submodule working tree.
#
# Patches here are bound to the pinned revision recorded in
# native/access-server/UPSTREAM.md. When the pin is updated, re-check every
# patch: drop it if the new pin contains the fix, rebase it otherwise.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
submodule="${repo_root}/third_party/fiber-gateway-cpp"
pinned="7e6930fc6b432c9f5575d969d14d249a2587dc0d"

cd "${submodule}"
rev="$(git rev-parse HEAD)"
if [ "${rev}" != "${pinned}" ]; then
    echo "native/patches are bound to fiber-gateway-cpp ${pinned}; refusing to apply on ${rev}" >&2
    echo "Rebase or drop the patches when updating the submodule pin." >&2
    exit 1
fi

status=0
for patch in "${repo_root}"/native/patches/*.patch; do
    [ -e "${patch}" ] || continue
    name="$(basename "${patch}")"
    if git apply --reverse --check "${patch}" >/dev/null 2>&1; then
        echo "already applied: ${name}"
    elif git apply --check "${patch}" >/dev/null 2>&1; then
        git apply "${patch}"
        echo "applied: ${name}"
    else
        echo "failed to apply: ${name}" >&2
        status=1
    fi
done
exit "${status}"
