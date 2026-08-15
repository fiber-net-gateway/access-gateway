# syntax=mirror.gcr.io/docker/dockerfile:1.7

FROM mirror.gcr.io/library/debian:trixie-slim AS native-build

ARG CMAKE_VERSION=4.1.2
ARG NATIVE_BUILD_JOBS=2

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        g++ \
        ninja-build \
        python3 \
        python3-pip \
    && python3 -m pip install --break-system-packages --no-cache-dir "cmake==${CMAKE_VERSION}"

WORKDIR /src
COPY third_party/fiber-gateway-cpp/ third_party/fiber-gateway-cpp/
COPY native/ native/

RUN cmake -S native -B native/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFIBER_BUILD_TESTS=OFF \
        -DFIBER_ENABLE_HTTP3=ON \
        -DFIBER_ENABLE_LTO=OFF \
    && cmake --build native/build \
        --target fiber_app_access_server fiber_app_access_gateway_validator \
        --parallel "${NATIVE_BUILD_JOBS}" \
    && strip native/build/apps/access-server native/build/apps/access-gateway-validator

FROM mirror.gcr.io/library/debian:trixie-slim AS access-server

RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates curl libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system access-gateway \
    && useradd --system --gid access-gateway --home-dir /nonexistent --no-create-home access-gateway

COPY --from=native-build /src/native/build/apps/access-server /usr/local/bin/access-server
COPY deploy/access-server/access-server.demo.conf /etc/access-gateway/access-server.demo.conf
COPY deploy/access-server/entrypoint.sh /usr/local/bin/access-server-entrypoint
RUN chmod 0755 /usr/local/bin/access-server-entrypoint

USER access-gateway:access-gateway
EXPOSE 16688/tcp 16688/udp 16689/tcp
ENTRYPOINT ["/usr/local/bin/access-server-entrypoint"]

FROM mirror.gcr.io/library/node:22-trixie-slim AS console-build

WORKDIR /app
COPY package.json package-lock.json tsconfig.base.json ./
COPY server/package.json server/package.json
COPY web/package.json web/package.json
RUN npm ci

COPY server/ server/
COPY web/ web/
COPY scripts/build-user-guide.mjs scripts/build-user-guide.mjs
COPY docs/user-guide/ docs/user-guide/
RUN npm run build

FROM mirror.gcr.io/library/node:22-trixie-slim AS console-production-deps

WORKDIR /app
COPY package.json package-lock.json ./
COPY server/package.json server/package.json
COPY web/package.json web/package.json
RUN npm ci --omit=dev --ignore-scripts \
    && npm cache clean --force

FROM mirror.gcr.io/library/node:22-trixie-slim AS console-runtime

ENV NODE_ENV=production
ENV CONSOLE_STATIC_ROOT=/app/web/dist
WORKDIR /app
COPY --from=console-production-deps --chown=node:node /app/node_modules/ node_modules/
COPY --from=console-build --chown=node:node /app/server/dist/ server/dist/
COPY --from=console-build --chown=node:node /app/web/dist/ web/dist/
COPY --chown=node:node server/package.json server/package.json
COPY --chown=node:node server/migrations/ server/migrations/
COPY --from=native-build /src/native/build/apps/access-gateway-validator /usr/local/bin/access-gateway-validator

USER node
EXPOSE 3000
CMD ["node", "server/dist/index.js"]

FROM console-runtime AS demo-runtime

USER root
COPY --from=native-build /src/native/build/apps/access-server /usr/local/bin/access-server
COPY deploy/access-server/access-server.demo.conf /etc/access-gateway/access-server.demo.conf
COPY deploy/access-server/entrypoint.sh /usr/local/bin/access-server-entrypoint
RUN chmod 0755 /usr/local/bin/access-server-entrypoint

USER node
