#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repository_root"

build_revision=$(git rev-parse --verify HEAD)
if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
    build_revision="${build_revision}-dirty"
fi

npm run build

cmake -S native -B native/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DFIBER_BUILD_TESTS=OFF \
    -DFIBER_ENABLE_HTTP3=ON \
    -DFIBER_ENABLE_LTO=OFF \
    -DACCESS_GATEWAY_BUILD_REVISION="$build_revision"
cmake --build native/build \
    --target fiber_app_access_server fiber_app_access_gateway_validator \
    --parallel "${NATIVE_BUILD_JOBS:-2}"

strip native/build/apps/access-server native/build/apps/access-gateway-validator

echo "Local Docker artifacts are ready"
