# Access Gateway

English | [简体中文](README.zh-CN.md)

Access Gateway is a self-hosted HTTP gateway made of two parts:

- **`access-server`**, a C++23 data plane built on the
  [Fiber](https://github.com/fiber-net-gateway/fiber-gateway-cpp) framework. It terminates client
  traffic, selects a route, and serves or proxies each request.
- **The Console**, a React and Fastify application where operators author, validate, release, and
  roll back configuration, then confirm which instances are actually running it.

Configuration travels from the Console to the data plane through rnacos (Nacos). The data plane
began as a migration of the Java `ploto-unified-access` gateway and keeps its configuration wire
format.

This README describes what the product does and how to run it. To understand how it works, read
[Architecture and principles](docs/architecture.md) (Chinese).

## Status

Access Gateway is under active development and is not yet cleared for production.

- **Data plane:** a limited 2026-07-31 test-environment snapshot passed 352/352 decode-and-compile
  checks, but production corpus differential verification and the final cutover gates are
  incomplete. The [cutover gate](native/access-server/docs/cutover-evidence-gate.md) status is
  `NOT_MET`.
- **Console:** the only working identity mode is a local development administrator, and document
  encryption uses a local key. `AUTH_MODE=oidc` refuses to start until a provider adapter exists.

See [Known limitations](#known-limitations) for the full list.

## Data plane features

### Listeners and protocols

| Listener       | Default            | Serves                                                     |
| -------------- | ------------------ | ---------------------------------------------------------- |
| Gateway TLS    | `0.0.0.0:8443/tcp` | HTTPS, with HTTP/1.1 and HTTP/2 negotiated through ALPN    |
| Gateway HTTP/3 | `0.0.0.0:8443/udp` | HTTP/3 over QUIC, advertised with `Alt-Svc`                |
| Plaintext      | `0.0.0.0:8000/tcp` | HTTP/1.1, alongside TLS or as the only traffic listener    |
| Status         | `0.0.0.0:8001`     | Prometheus `/metrics` and optional activation evidence API |

HTTP worker count follows the process CPU affinity and cgroup v1/v2 quota automatically.

### Projects and hosts

A Project owns a set of Host patterns and an ordered list of routes. Hosts are matched
case-insensitively after removing the port and one trailing dot, and each pattern belongs to exactly
one Project.

| Pattern         | Example           | Matches                                                  |
| --------------- | ----------------- | -------------------------------------------------------- |
| Exact           | `api.example.com` | That host only                                           |
| Suffix wildcard | `*.example.com`   | `a.example.com` and `a.b.example.com`, not `example.com` |
| Global wildcard | `*`               | Any valid host                                           |

Project selection uses HTTP `Host` or `:authority`. It is independent of the TLS SNI value, so one
certificate can serve several Projects and vice versa.

### Route matching

Routes match on path structure first, then on an optional method and condition.

| Path form      | Example         | Captures                       |
| -------------- | --------------- | ------------------------------ |
| Static segment | `/users/me`     | Nothing                        |
| Parameter      | `/users/:id`    | One segment, as `$path.id`     |
| Tail wildcard  | `/assets/*rest` | The remainder, as `$path.rest` |

Static segments beat parameters, and parameters beat wildcards. Candidates on the same path are
tried in configured order. A `condition` is a compiled expression that can read `$path`, `$query`,
`$header`, `$cookie`, `$context`, `$req`, and `$conn`:

```yaml
path: /api/orders/:id
method: GET
condition: '$header.x_tenant == "acme"'
type: PROXY
service: orders/stable
```

### Route types

**RESPONSE** answers directly from the gateway, with no upstream. The body can be `TEXT`, `BASE64`
(decoded at compile time), or `TEMPLATE` (`${...}` expressions evaluated per request).

```yaml
path: /healthz
method: GET
type: RESPONSE
status: 200
body:
  type: TEXT
  content: ok
response_headers:
  Content-Type: text/plain; charset=utf-8
```

**PROXY** forwards to an upstream service, with templated path rewriting and headers.

```yaml
path: /api/orders/:id
type: PROXY
service: orders/stable
timeout: 30s
rewrite: '/internal/orders/${$path.id}'
proxy_headers:
  X-Tenant: '${$header.x_tenant}'
response_headers:
  Cache-Control: no-store
```

**SCRIPT** runs a JavaScript-like program compiled when the configuration loads. Scripts can read
the request and JSON or binary bodies, set headers and cookies, send responses, and call fixed HTTP
backends through an `http` directive. The standard library covers strings, arrays, binary, hashes,
math, randomness, JSON, and URLs. This is not Node.js: there is no filesystem, timers, modules, or
user-defined functions.

<!-- prettier-ignore -->
```javascript
return {
    id: $path.id,
    method: $req.method,
    source: $query.source || "unknown"
};
```

### Proxying

- **Upstreams:** NamingService discovery (`service`, with an optional `/cluster` suffix) or a static
  `addresses` list. Discovery uses only enabled, healthy, positively weighted instances and prefers
  the local zone.
- **Load balancing:** smooth weighted round robin with a per-endpoint circuit breaker.
- **Retries:** a failed connection is re-selected, up to four attempts in total, only while the
  request head has not started sending, so a request is never delivered twice.
- **Connections:** connection pooling, a bounded DNS resolver with no implicit public fallback, and
  Happy Eyeballs across resolved addresses under one connect deadline.
- **Rewriting:** templated `rewrite`, `proxy_headers`, and `response_headers`, with the original
  URI and percent-encoding preserved when no rewrite is set. `Location` and `Refresh` are rewritten
  automatically unless a route overrides them.
- **Streaming:** bodies stream in both directions. `flush: true` lowers latency for SSE and slow
  uploads.
- **WebSocket:** routes with `websocket_timeout` tunnel an HTTP/1.1 upgrade as raw bytes.
- **Limits:** per-route `max_client_body_size`, `max_proxy_body_size`, and `timeout`, falling back
  to the server defaults of 400 MiB for requests and 600 MiB for proxied responses.

### Upstream TLS

The process-wide default is `ACCESS_SERVER_UPSTREAM_TLS_MODE`: `legacy_insecure` (the default, which
does not verify certificates), `system_ca`, or `custom_ca`. Use a verifying mode in production.

A PROXY route can override it with an immutable `upstream_tls` profile that sets the verification
mode, an inline CA, the SNI name, a separate verification name, and a client certificate for mTLS.
The client certificate is referenced by a Console certificate version UUID, so private keys never
appear in route configuration.

### Response compression

`gzip: true` (level 6) or `gzip: 1`–`9` enables compression on any route type. Static `TEXT` and
`BASE64` bodies are precompressed at compile time; template, proxy, and script responses compress as
a stream. The gateway negotiates on `Accept-Encoding`, applies a fixed MIME allow-list, merges
`Vary`, and returns 406 when every representation is refused.

### Access policies

| Policy         | Scope | Behavior                                                                             |
| -------------- | ----- | ------------------------------------------------------------------------------------ |
| HTTPS redirect | Host  | Redirects with 301, 302, 307, or 308 and sends `Strict-Transport-Security`           |
| Entry network  | Host  | Rejects with 403 unless the instance entry (`vdi`, `desktop`, `internet`) is allowed |
| Source CIDR    | Route | `allows` holds allow and `!deny` entries, IPv4 and IPv6; deny wins, 403              |
| Request body   | Route | Rejects oversized bodies with 413, for both declared and streamed lengths            |

HTTPS redirects fire on the plaintext listener. Behind a reverse proxy, the client address and
scheme come from `ACCESS_SERVER_CLIENT_METADATA_MODE`:

- `direct` (default) uses the socket peer and the listener's real TLS state and ignores all
  forwarding headers.
- `trusted_proxy` parses `Forwarded`, `X-Forwarded-For`, and `X-Real-Ip` only when the peer is in
  `ACCESS_SERVER_TRUSTED_PROXY_CIDRS`. An invalid chain falls back to the socket peer.
- `legacy_headers` keeps the Java behavior of trusting these headers and skipping the CIDR check
  when `X-Real-Ip` is missing or invalid. Do not use it where untrusted clients can reach the
  listener.

### Gray release and clusters

Gray rules in `ploto.unified-access.gray-match` map an entry network to a ratio (out of 10,000) and
a CIDR list. A matching request is routed to the `gray` cluster. Clusters can also come from a
route's `service/cluster` value, its `cluster` field, or its `context` map. When a
request-specified cluster has no usable instance, the gateway falls back to the configured one.

With `ACCESS_SERVER_TEST_MODE=true`, the host form `api_gray.example.com` routes as
`api.example.com` in cluster `gray`, and `HI-TRACE-CLUSTER` selects a cluster when the host names
none. Gray rules do not apply in this mode.

### Edge functions

Edge functions are scripts mounted on a set of Projects. They can inspect a request and answer it
before it reaches the route's normal handling. They power the WAF and can also be written by hand.

- **Phases:** `request_head` runs before route matching, so unmatched paths are covered too.
  `request_body` runs after matching and reads a buffered `$req.body`, only on PROXY routes with
  `edge_function_body: true`, for JSON, form, text, and XML content types, within a 1 MiB cap.
- **Mounting:** each function has project `include` and `exclude` globs, an execution `priority`,
  and a sticky sampling `ratio` keyed on the client IP.
- **Version canary:** `replaces` names the function a new version takes over inside its `ratio`.
  Each request runs exactly one of the two, the old function's configuration stays untouched, and
  removing the new function restores it. Behind a CDN or load balancer, set
  `ACCESS_SERVER_CLIENT_METADATA_MODE` so sampling sees real clients rather than the intermediary.
- **Capabilities:** read the request, respond early through `resp.*`, count events with
  `ctx.report(event, label)`, and test addresses with a `cidr` directive. Functions cannot modify
  the request or call upstreams.
- **Failure:** a runtime error fails open by default. Set
  `ACCESS_SERVER_EDGE_FUNCTION_FAIL_CLOSE=true` to reject with 503 instead.

```javascript
directive office = cidr "10.0.0.0/8;172.28.1.0/24";

if (!office.contains($conn.remote_addr) && strings.match($req.uri, "(?i)union\\s+select")) {
  ctx.report("waf.block", "sqli-0023");
  resp.send(403, "blocked");
}
```

### Hot configuration updates

Routes, Project lists, gray rules, TLS certificates, and edge functions all update without a
restart.

- A candidate is fully parsed and compiled before it replaces the running snapshot.
- An invalid or oversized candidate is rejected, and the previous snapshot keeps serving.
- A candidate with the same version is ignored.
- In-flight requests finish on the snapshot they started with.
- Removing a Project from the list unloads it, which is distinct from a failed update.

Certificates are selected at the ClientHello by SNI, using an index built from each leaf
certificate's DNS SANs. Rotating a certificate is an atomic snapshot swap.

### Observability

- **Prometheus:** a fixed-schema metric set covers requests, latency, per-route and per-upstream
  traffic, proxy errors, circuit rejections, configuration outcomes, TLS rotation, DNS, service
  discovery, WebSocket, and edge functions. Request-labeled families are capped with LRU eviction.
  See [bounded metrics](native/access-server/docs/bounded-metrics.md).
- **Grafana:** two dashboards are in [`deploy/grafana/`](deploy/grafana/): Request Handling and
  Runtime Health.
- **Tracing:** CAT request trees, `HI-TRACE-ID` continuation, and W3C `traceparent` and `tracestate`
  propagation.
- **Access log:** a Java-compatible, tab-separated line per proxy attempt, written asynchronously
  and rotated at 100 MB. A full log queue drops lines rather than blocking requests.
- **Edge function events:** a separate structured log line for each function event.

### Upgrades

`access-server-boss` supervises the process and `access-serverctl` drives prewarmed upgrades. The
new generation syncs configuration and initializes workers while the old one still serves, then
takes over the TCP listeners. This shrinks the gap in service but does not remove it, and HTTP/3
requests can fail while the two processes overlap. See the
[upgrade guide](docs/access-server-prewarm-upgrade-design.md).

## Console features

### Projects and routes

A Project is a normalized domain name. Each route gets its own CodeMirror editor, written either as
a YAML mapping or as a JavaScript route with path, method, and gzip set outside the source. Routes
can be reordered by drag or keyboard. A preview shows the exact native wire output before anything
is saved, and native resource limits appear in the editor.

### Configuration versions

Saving creates a new immutable, encrypted Configuration Version, protected by `If-Match` optimistic
locking and idempotency keys. Any version can be previewed, restored, used as the starting point for
new edits, or published directly.

### Host and network policy

- **Host Policy:** a fixed primary domain plus exact host aliases that share its routes and Release,
  and an HTTPS redirect mode.
- **Network Policy:** Project-wide allow and deny CIDR lists, compiled into every route.

### Validation

Configuration is checked in three stages: field syntax, relationships across resources, then full
compilation by the Native Validator, which is the same codec and compiler the data plane runs.
Creating a Release requires a working validator, so an unverified configuration cannot be published.

### Releases and publication

- A Release is created from the current version or any historical one and is stored immutably before
  publishing. Rolling back means releasing an older version.
- A leased worker writes each rnacos Data ID, reads it back, and records every attempt separately.
- If rnacos has drifted from what the Release was based on, publishing stops with a conflict instead
  of overwriting.
- Decommissioning a Project is its own Release type, requiring a recorded reason.

### Activation evidence

Publishing is not the same as being active, so the Console tracks them separately. A collector polls
each instance's authenticated `/v1/activation-evidence` endpoint and aggregates the results onto the
Release page, with per-instance detail on demand.

| Status     | Meaning                                                                     |
| ---------- | --------------------------------------------------------------------------- |
| `active`   | Every required instance has fresh evidence that exactly matches the Release |
| `pending`  | Evidence is fresh, but the target has not reached the active snapshot yet   |
| `degraded` | Collection failed, or an instance rejected the candidate                    |
| `unknown`  | Evidence is missing or expired, which is never presented as success         |

### TLS certificates

- **Inventory:** encrypted certificates with immutable versions and a `valid`, `expiring` (within 30
  days), `expired`, or `superseded` state. There is no private key download API.
- **Automatic SNI:** selectors come from each certificate's DNS SANs, with no separate binding to
  maintain. A preview shows which certificate a given server name resolves to.
- **Renewal:** an update with the same SANs switches directly. Adding or removing a SAN changes SNI
  coverage and requires explicit confirmation.
- **TLS Releases:** a complete certificate snapshot is published and tracked like any other Release.

### WAF

WAF rules are authored in the Console and compile into edge functions, so the data plane has no
WAF-specific logic.

- **Rule groups:** each group has a mode (`off`, `log`, or `block`), a priority, and included and
  exempted Projects. An exemption requires a written reason. Each active group publishes as one edge
  function named `waf-<group>`.
- **Rules:** a match expression with severity, action, and an optional scope of methods, paths, and
  source CIDRs.
- **Project overrides:** a per-Project mode that overrides the groups it belongs to.
- **Starter rule pack:** nine rules in six groups (SQL injection, RCE, XSS, path traversal, probing,
  malformed requests). Groups import as `off`, so importing changes nothing until they are enabled.
- **Blocking:** a blocked request gets a 403 JSON body with code `WAF_BLOCKED` and the rule ID.
- **Publishing:** a compile preview checks the generated scripts, and a release preview compares them
  function by function against what is live in rnacos. Rollback creates a new Release from historical
  content.
- **Hit statistics:** per-rule `log` and `block` counts over 1 hour, 24 hours, or 7 days, read from
  Prometheus. This page requires `PROMETHEUS_URL`.

The recommended rollout is one Project in `log` mode, then review the hit statistics, then switch to
`block`.

### Handwritten edge functions

A dedicated page edits function scripts, phases, and mounts, with per-function native compile
previews. It starts a version canary from an existing function and promotes it once it owns all the
traffic. Handwritten and WAF-generated functions publish together. The names `waf` and `waf-*` are
reserved for the generator, and a snapshot can hold up to 64 functions in total.

### Access control and audit

- **Roles:** `admin` and `maintainer` edit configuration; `admin` and `publisher` create and publish
  Releases; `auditor` can read but not change anything.
- **Audit:** configuration, certificate, and publication changes write redacted audit events.
- **Encryption:** configuration documents and private keys are stored with AES-256-GCM envelope
  encryption.

### Built-in documentation

The route, script, and script API guides are served in the Console under `/docs`, in English and
Simplified Chinese.

## Known limitations

**Console**

- OIDC is not implemented, so production login is unavailable. The local document key is for
  development only; production needs an external key provider, which does not exist yet.
- Gray rules and Host entry-network restrictions are not editable in the Console. Author them
  directly in rnacos.
- A failed or partial publication is final, with no automatic per-resource retry. Recover by
  creating a new Release.
- TLS Releases have no one-click rollback, unlike WAF Releases.
- Audit events are written to the database, but there is no viewer or API yet.
- Role memberships have no management page or API.
- Script Data IDs of removed edge functions stay in rnacos as harmless orphans.

**Data plane**

- Upstream connections are HTTP/1.1 only, and WebSocket requires an HTTP/1.1 client upgrade.
- JavaScript routes reject request bodies of unknown length with 413, and their `http` directive
  does not resolve NamingService upstreams.
- Edge functions cannot rewrite requests, and the script VM has no instruction budget. Static
  language restrictions and input limits are the current safeguards.
- `/metrics` has no authentication, so keep the status listener on a private network.
- Changing the DNS resolver file, the process CA bundle, or the activation token requires a rolling
  restart.

## Quick start: Docker demo

The demo runs the full stack: Console and API, publication worker, activation collector, MySQL 8.4,
R-Nacos, and `access-server`. It needs Docker Compose, OpenSSL, and the
[development prerequisites](#prerequisites), because the binaries are compiled locally first.

```bash
git clone --recurse-submodules https://github.com/fiber-net-gateway/access-gateway.git
cd access-gateway
npm install
npm run demo:init   # generate local secrets and a self-signed certificate
npm run demo:up     # build locally, then start the stack
```

The first build downloads native dependencies and can take several minutes; `NATIVE_BUILD_JOBS`
sets its parallelism (default 2). Compose applies migrations, uploads the demo certificate, publishes
a TLS snapshot and a demo Project through the Console, and starts `access-server` once both are in
R-Nacos.

| Service             | Address                                                         |
| ------------------- | --------------------------------------------------------------- |
| Console             | `http://localhost:8088`                                         |
| Gateway HTTPS/HTTP2 | `curl -k --http2 -H 'Host: demo.local' https://localhost:8443/` |
| Gateway plaintext   | `curl -H 'Host: demo.local' http://localhost:8000/`             |
| Metrics             | `http://localhost:8001/metrics`                                 |
| R-Nacos console     | `http://localhost:10848/rnacos/`                                |
| MySQL               | `127.0.0.1:3307`                                                |

Credentials are in the ignored local `.env` file. Operate the stack with `npm run demo:ps`,
`npm run demo:logs`, and `npm run demo:down`. Add `--volumes` to `docker compose down` only to delete
all demo data.

To test HTTP/3 from a Windows browser under WSL2, use the WSL address from `hostname -I`, because
WSL2 forwards TCP but usually not UDP from Windows `localhost`. Map it to `demo.local` in the Windows
hosts file and trust the demo certificate. Never reuse this certificate outside local testing. Set
`ACCESS_SERVER_PUBLISHED_HOST=127.0.0.1` in `.env` to keep the gateway inside WSL.

## Development

### Prerequisites

- **Console:** Node.js 20.19 or later, with npm workspaces.
- **Native (Linux):** CMake 4.1 or later, Ninja, and Clang 17+ or GCC 13+ with C++23 support.

Clone with `--recurse-submodules`, or run `git submodule update --init --recursive` in an existing
checkout. Do not use `git submodule update --remote`: Fiber is pinned, and updates are deliberate,
reviewed gitlink changes.

### Console

```bash
npm install
cp server/.env.example server/.env
npm run dev
```

This starts the Console at `http://localhost:5173` and the API at `http://127.0.0.1:3000`, with Vite
proxying `/api`. Use `npm run dev:web` or `npm run dev:server` to run one side.

Without MySQL, persistent endpoints return `503` and `/api/health/ready` reports degraded. To enable
persistence, create a MySQL 8.4 database, then set `MYSQL_ENABLED=true`, `MYSQL_PASSWORD`, and
`DOCUMENT_ENCRYPTION_KEY_BASE64` (from `openssl rand -base64 32`) in `server/.env`:

```bash
npm run db:migrate
npm run dev
```

The API never runs migrations on its own. A fresh database has no workspace, so create one with a
single `POST /api/environments`; [`deploy/demo/bootstrap-console.mjs`](deploy/demo/bootstrap-console.mjs)
shows the request.

| Capability         | Setting                                                                                                                    |
| ------------------ | -------------------------------------------------------------------------------------------------------------------------- |
| Native validation  | `NATIVE_VALIDATOR_PATH`, the absolute path to `access-gateway-validator`                                                   |
| Publication worker | `PUBLICATION_WORKER_ENABLED=true`, run with `npm run publication-worker --workspace server`                                |
| Activation         | `ACTIVATION_COLLECTOR_ENABLED=true`, `ACTIVATION_TARGETS_JSON`, run with `npm run activation-collector --workspace server` |
| WAF hit statistics | `PROMETHEUS_URL`                                                                                                           |

The worker and collector run from the build output, so run `npm run build` first. See
[`server/.env.example`](server/.env.example) for every setting.

### Native

```bash
npm run configure:native
npm run build:native             # native/build/apps/access-server
npm run build:native-validator   # native/build/apps/access-gateway-validator
npm run test:native
```

Further commands: `npm run test:native:sanitizers`, `npm run test:native:interop`,
`npm run benchmark:native`, and `npm run verify:native:cutover`.

### Validation

| Changed scope           | Run                                                                      |
| ----------------------- | ------------------------------------------------------------------------ |
| `web/`, `server/`       | `npm run typecheck`, `npm test`, `npm run format:check`, `npm run build` |
| `native/access-server/` | `npm run test:native`, `npm run format:native`, and review the diff      |

The full matrix and all coding conventions are in [AGENTS.md](AGENTS.md).

## Running access-server

```bash
cp native/access-server/access-server.env.example access-server.env
# Set NACOS_SERVER_ADDRESSES and ACCESS_SERVER_NETWORK_ENTRY (see below).
./native/build/apps/access-server access-server.env
```

Without an argument, the process reads `access-server.env` from the current directory. The format
is strict `KEY=VALUE`, and unknown or duplicate keys fail startup. Do not commit this file.

Before serving, the process waits up to `ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS` for a valid
Project list and every route it references, plus a valid certificate snapshot when TLS is enabled.
It then binds its listeners, registers in Nacos, and starts accepting once registered. Missing or
invalid configuration, an expired certificate, or a failed bind stops startup.

| Area           | Settings                                                                                                               |
| -------------- | ---------------------------------------------------------------------------------------------------------------------- |
| Required       | `NACOS_SERVER_ADDRESSES` (IP literals), `ACCESS_SERVER_NETWORK_ENTRY`                                                  |
| Listeners      | `ACCESS_SERVER_TLS_*`, `ACCESS_SERVER_HTTP3_ENABLED`, `ACCESS_SERVER_PLAIN_LISTEN_*`, `ACCESS_SERVER_METRICS_LISTEN_*` |
| Client trust   | `ACCESS_SERVER_CLIENT_METADATA_MODE`, `ACCESS_SERVER_TRUSTED_PROXY_CIDRS`                                              |
| Upstreams      | `ACCESS_SERVER_UPSTREAM_*`, `ACCESS_SERVER_HAPPY_EYEBALLS_*`, `ACCESS_SERVER_DNS_*`                                    |
| Limits         | `ACCESS_SERVER_MAX_REQUEST_BODY_SIZE`, `ACCESS_SERVER_MAX_RESPONSE_BODY_SIZE`                                          |
| Edge functions | `ACCESS_SERVER_EDGE_FUNCTION_FAIL_CLOSE` (default `false`)                                                             |
| Activation     | `ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED`, `ACCESS_SERVER_INSTANCE_ID`, `ACCESS_SERVER_ACTIVATION_TOKEN`             |
| Registration   | `ACCESS_SERVER_SERVICE_NAME`, `ACCESS_SERVER_CLUSTER`, `ACCESS_SERVER_ADVERTISE_ADDRESS`, `ACCESS_SERVER_ZONE`         |
| Logging, CAT   | `ACCESS_SERVER_ACCESS_LOG_DIR`, `CAT_*`                                                                                |

`ACCESS_SERVER_NETWORK_ENTRY` declares the entry network this instance serves (for example
`internet`, `vdi`, or `desktop`). The process refuses to start without it. The instance sets
`X-Entry`, `X-Real-Ip`, `X-Forwarded-Proto`, and `X-Forwarded-For` from this value and the connection,
replacing any client-supplied `X-Entry`. The `internet` entry also trusts `EO-Connecting-IP` from the
CDN for `X-Real-Ip`.

To run plaintext only, set `ACCESS_SERVER_TLS_ENABLED=false` and
`ACCESS_SERVER_HTTP3_ENABLED=false`.

See [`access-server.env.example`](native/access-server/access-server.env.example) for defaults and
per-setting notes.

## rnacos contract

| Purpose                 | Data ID                                     | Group           |
| ----------------------- | ------------------------------------------- | --------------- |
| Project list            | `ploto.unified-access.projects`             | `ACCESS-SERVER` |
| Project route           | `ploto.unified-access.route.<project>`      | `ACCESS-SERVER` |
| TLS certificates        | `ploto.unified-access.tls-certificates`     | `ACCESS-SERVER` |
| Edge function index     | `ploto.unified-access.edge-functions`       | `ACCESS-SERVER` |
| Edge function script    | `ploto.unified-access.edge-function.<name>` | `ACCESS-SERVER` |
| Gray rules              | `ploto.unified-access.gray-match`           | `DEFAULT_GROUP` |
| Test cluster allow-list | `ploto.test.temp-cluster-apps`              | `DEFAULT_GROUP` |

The Project list is a semicolon-separated string, not JSON. The native codec is the source of truth
for accepted values and defaults. `ploto.test.temp-cluster-apps` is read only when
`ACCESS_SERVER_TEST_MODE=true`.

## Repository layout

```text
access-gateway/
├── native/access-server/        # C++23 data plane, tests, and native docs
├── server/                      # Fastify control-plane API, worker, and collector
├── web/                         # React and Vite console
├── deploy/                      # Dockerfile, demo bootstrap, Grafana dashboards
├── docs/                        # Architecture, design docs, and user guides
├── third_party/fiber-gateway-cpp/  # Pinned, read-only Fiber submodule
└── AGENTS.md                    # Development conventions
```

## Documentation

**Start here**

- [Architecture and principles](docs/architecture.md)
- [User guide index](docs/user-guide/README.md), in English and Simplified Chinese:
  [routing](docs/user-guide/en/routing.md), [script routes](docs/user-guide/en/script-routing.md),
  and the [script reference](docs/user-guide/en/script-reference.md)

**Design**

- [Edge functions and WAF](docs/edge-functions-and-waf-design.md)
- [Instance activation evidence](docs/activation-evidence.md)
- [Network policy and certificates](docs/network-policy-and-certificate-design.md)
- [Prewarm upgrade](docs/access-server-prewarm-upgrade-design.md)
- [Console requirements](docs/console-requirements.md) and
  [detailed design](docs/console-detailed-design.md)

**Data plane**

- [Access Server guide](native/access-server/README.md)
- [Compatibility contract](native/access-server/docs/compatibility-contract.md)
- [Configuration compilation](native/access-server/docs/config-compilation.md) and
  [resource limits](native/access-server/docs/config-resource-limits.md)
- [Bounded metrics](native/access-server/docs/bounded-metrics.md)
- [Benchmark baselines](native/access-server/docs/benchmark-baselines.md)
- [Migration plan](native/access-server/docs/migration-plan.md) and
  [cutover evidence gate](native/access-server/docs/cutover-evidence-gate.md)
- [Upstream provenance](native/access-server/UPSTREAM.md)

## License

Licensed under the [Apache License 2.0](LICENSE). The migrated application retains its
[upstream license notice](native/access-server/LICENSE.upstream).
