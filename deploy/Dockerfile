# syntax=mirror.gcr.io/docker/dockerfile:1.7

FROM mirror.gcr.io/library/debian:trixie-slim AS access-server

RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates curl libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system access-gateway \
    && useradd --system --gid access-gateway --home-dir /nonexistent --no-create-home access-gateway

COPY native/build/apps/access-server /usr/local/bin/access-server
COPY deploy/access-server/access-server.demo.conf /etc/access-gateway/access-server.demo.conf
COPY deploy/access-server/entrypoint.sh /usr/local/bin/access-server-entrypoint
RUN chmod 0755 /usr/local/bin/access-server-entrypoint

USER access-gateway:access-gateway
EXPOSE 8443/tcp 8443/udp 8000/tcp 8001/tcp
ENTRYPOINT ["/usr/local/bin/access-server-entrypoint"]

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
COPY --chown=node:node server/dist/ server/dist/
COPY --chown=node:node web/dist/ web/dist/
COPY --chown=node:node server/package.json server/package.json
COPY --chown=node:node server/migrations/ server/migrations/
COPY native/build/apps/access-gateway-validator /usr/local/bin/access-gateway-validator
RUN ldd /usr/local/bin/access-gateway-validator \
    && ! ldd /usr/local/bin/access-gateway-validator | grep -q 'not found'

USER node
EXPOSE 3000
CMD ["node", "server/dist/index.js"]

FROM console-runtime AS demo-runtime

USER root
RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
COPY native/build/apps/access-server /usr/local/bin/access-server
COPY deploy/access-server/access-server.demo.conf /etc/access-gateway/access-server.demo.conf
COPY deploy/access-server/entrypoint.sh /usr/local/bin/access-server-entrypoint
RUN chmod 0755 /usr/local/bin/access-server-entrypoint \
    && ldd /usr/local/bin/access-server \
    && ! ldd /usr/local/bin/access-server | grep -q 'not found'

USER node
