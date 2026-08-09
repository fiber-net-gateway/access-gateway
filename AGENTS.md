# Repository Guidelines

## Product Scope

This repository owns the complete Access Gateway product. It has two first-class, actively
developed parts:

- `native/access-server/`: the C++23 data plane migrated from
  `fiber-net-gateway/fiber-gateway-cpp/apps/access-server`;
- `web/` and `server/`: the management console frontend and control-plane API.

The native application is not a frozen import. Continue to implement data-plane features, fixes,
compatibility behavior, performance work, and operational improvements in this repository. The
console is not the sole development target.

`access-server` selects projects and routes, applies Host/Path/entry/CIDR/gray policies, serves
configured responses, proxies HTTP and WebSocket traffic, consumes rnacos configuration and
NamingService data, and emits metrics, traces, and structured access logs. The console manages the
configuration and release workflow around that runtime. It must not become a second traffic proxy
or a generic rnacos administration console.

Keep lifecycle states distinct in storage, APIs, metrics, and UI:

1. a draft saved by the console;
2. content published to rnacos;
3. content proven active on a particular access-server instance.

A successful rnacos write or readback is not activation evidence. Until the data plane exposes
typed per-instance evidence, report activation as unknown rather than inferring success.

## Current State and Direction

The C++ data plane is maintained in `native/access-server/` and wired to the pinned Fiber submodule
through `native/CMakeLists.txt`. Its application and focused CTest targets are active. Production
script-corpus differential verification and final cutover gates remain unfinished; preserve that
qualification in user-facing documentation until the gates are complete.

The `web` and `server` npm workspaces provide a React/Vite shell, a Fastify API, deterministic
environment parsing, tests, and `/api/health` connectivity. Authentication, database persistence,
rnacos publication, environment management, business editors, and instance activation evidence
are not yet implemented. UI and APIs must label these states as unavailable or unknown rather than
simulating them.

Develop both components in coherent vertical increments. A configuration feature is incomplete if
the console can author data that the native codec cannot consume, or if native wire behavior changes
without corresponding control-plane validation, documentation, and compatibility coverage.

## Source Ownership and Upstream Boundaries

- `native/access-server/` is repository-owned application code. Implement access-server product
  work there, including its `src/`, `tests/`, `docs/`, fixtures, scripts, environment example, and
  CMake target definitions.
- `third_party/fiber-gateway-cpp/` is the canonical read-only upstream reference and the pinned
  source for reusable Fiber runtime, HTTP, script/JSON, Nacos, CAT, and Prometheus targets. Native
  CMake must consume this checkout rather than downloading a second moving copy.
- Do not build or import the upstream `apps/access-server` as another application target. The
  maintained application is the copy under `native/access-server/`.
- Do not edit files inside the submodule as part of a product change. Put application behavior in
  `native/access-server/`. Contribute reusable framework or library fixes upstream, then update the
  gitlink deliberately after the upstream change is available.
- Do not use `git submodule update --remote` in routine setup or builds. A gitlink update requires
  review of the upstream revision range, provenance updates where applicable, and native regression
  testing.
- If a temporary upstream compatibility patch is unavoidable, keep it narrow under
  `native/patches/`, bind it to the pinned revision, document the upstream Issue or PR, and remove it
  once the dependency pin contains the fix.
- Keep the import revision recorded in `native/access-server/UPSTREAM.md` and preserve
  `native/access-server/LICENSE.upstream`. The initial import/submodule revision is
  `0fda7764bf94944aca4b674ab5ab311184703118`.
- Preserve the Java compatibility baseline documented for access-server:
  `ploto-gateway` commit `22c2bf543b96b52c0ccecd4ceb07d4911c502f45`. Before changing the
  baseline, update compatibility docs, fixtures, and differential records.

When deciding whether code belongs in this repository or upstream, use this boundary: gateway
product policy and Java compatibility belong in `native/access-server`; generally reusable event,
HTTP, client, pool, protocol, observability, or framework behavior belongs in
`fiber-gateway-cpp`.

## Repository Layout

- `web/`: React, TypeScript, and Vite management frontend. Put reusable UI in
  `web/src/components/`, route-level screens in `web/src/pages/`, and typed API access in
  `web/src/api/`.
- `server/`: Node.js, TypeScript, and Fastify control-plane API. Keep application construction
  separate from process startup so tests can use Fastify injection without opening sockets.
- `server/src/config/`: validated environment parsing. Do not scatter `process.env` reads.
- `server/src/database/`: connection management and deterministic migrations.
- `server/src/modules/<domain>/`: domain routes, schemas, services, repositories, and colocated
  tests. Expected domains include environments, projects/routes, gray rules, releases, users, and
  publication evidence.
- `native/CMakeLists.txt`: top-level native build and pinned Fiber integration.
- `native/access-server/src/`: application modules grouped by responsibility: `config/`,
  `routing/`, `execution/`, `runtime/`, and `observability/`.
- `native/access-server/tests/`: GoogleTest coverage and Java compatibility fixtures.
- `third_party/fiber-gateway-cpp/`: pinned submodule; never place repository-owned code in it.
- `deploy/`, root container files, and `compose.yaml`: reproducible end-to-end deployment.
- `docs/`: cross-component architecture, release, deployment, and operational decisions.

Generated and private paths such as `node_modules/`, all `dist/` directories, `native/build*/`,
local database data, and environment or credential files must remain ignored and uncommitted.

## Component and Configuration Boundaries

- `access-server` is the only component that receives or proxies gateway traffic. The console API
  manages configuration and operations; it must not forward ordinary gateway requests.
- The control plane must not access data-plane memory, and access-server must not read console
  database tables. Runtime configuration crosses the boundary through the established rnacos wire
  contract. Add operational status through explicit, authenticated, bounded APIs or service
  discovery.
- Keep database, rnacos, and access-server clients behind typed services. HTTP handlers must not
  contain SQL, rnacos protocol details, or ad hoc runtime calls.
- Unit-test construction must not require MySQL, rnacos, CAT, upstream services, public networks,
  or wall-clock timing. Open external connections only in explicit lifecycle steps and close them
  in reverse dependency order.
- Treat project/route publication as a multi-resource workflow, not as a transaction rnacos does
  not provide. Persist an immutable release before publication and record every Data ID write,
  readback, failure, and later instance evidence independently. Rollback creates a new release from
  historical content; it never rewrites release history.
- Validate console configuration in stages: field syntax, relationships across resources, then the
  complete environment. The repository-owned native codec and compiled route model are the source
  of truth for fields, defaults, scalar coercion, semantic validation, and failure-old-snapshot
  behavior.
- Never bypass access-server version semantics. Same-version route candidates are ignored, invalid
  candidates retain the prior snapshot, and removing a project or Host mapping is different from a
  failed update.

Preserve these default rnacos contracts unless a compatibility change is explicitly documented and
tested:

| Purpose               | Data ID                                | Group           |
| --------------------- | -------------------------------------- | --------------- |
| Project list          | `ploto.unified-access.projects`        | `ACCESS-SERVER` |
| Project route         | `ploto.unified-access.route.<project>` | `ACCESS-SERVER` |
| Production gray rules | `ploto.unified-access.gray-match`      | `DEFAULT_GROUP` |

The project list is a semicolon-separated string, not JSON. Per-project routes and gray rules must
retain the native compatibility codec's wire behavior. A normalized console model may be richer
internally, but publication must compile it deterministically into this format.

Any change to the wire model, defaults, validation, routing behavior, publication semantics, or
activation evidence is cross-component work. Update the native codec/runtime and tests first or in
the same change, then update server schemas/services, frontend types and editors, fixtures, and
user documentation as applicable.

## Security and Secrets

Nacos credentials, database passwords, session/signing secrets, CAT configuration secrets,
authorization headers, cookies, request/response bodies, and sensitive route header values must
never be logged, committed, included in plaintext diffs, or returned after one-time secret
delivery. Redact secrets in API responses, audit events, error messages, access logs, traces, and
operational diagnostics.

Treat configuration editors and proxy metadata as untrusted input. Validate at the API boundary,
encode at the output boundary, use parameterized SQL, constrain URLs and header names, and preserve
the native runtime's protected-header rules. Do not weaken fail-closed startup or routing behavior
to make local development appear successful.

## Setup and Common Commands

Initialize the pinned dependency before native work:

```bash
git submodule update --init --recursive
```

Run project commands from the repository root. The console workflow is:

```bash
npm install
cp server/.env.example server/.env
npm run dev
```

Useful root commands are:

- `npm run dev`: start Vite and Fastify in watch mode.
- `npm run dev:web` / `npm run dev:server`: start one console workspace.
- `npm run typecheck`: run strict TypeScript checking in all workspaces.
- `npm test`: run configured frontend and backend tests.
- `npm run format` / `npm run format:check`: apply or verify Prettier formatting.
- `npm run build`: build both console workspaces.
- `npm run configure:native`: configure the Release native build with tests.
- `npm run build:native`: build `fiber_app_access_server`.
- `npm run test:native`: build and run focused access-server tests.

The expanded native workflow is:

```bash
cmake -S native -B native/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_BUILD_TESTS=ON
cmake --build native/build --target fiber_app_access_server --parallel
cmake --build native/build --target fiber_access_server_tests --parallel
ctest --test-dir native/build --output-on-failure -L access-server
```

The application target is `fiber_app_access_server`, its output name is `access-server`, and the
focused test target is `fiber_access_server_tests`.

## TypeScript, React, and API Conventions

Use strict TypeScript. Repository Prettier settings require two-space indentation, single quotes,
no semicolons, trailing commas, and a 100-character print width. Use `PascalCase` for React
components and types, `camelCase` for functions and variables, and kebab-case for CSS classes.

Use functional React components and data-driven rendering for repeated UI. Preserve accessible
labels, keyboard operation, responsive layouts, reduced-motion behavior, and unsaved-change
guards. Do not use color as the only distinction between environments, lifecycle states, failures,
or activation status.

Backend routes live under `/api`. Define explicit TypeScript types and Fastify schemas for request,
response, path, and query data. Return stable machine-readable error codes and field paths for
validation errors. Keep authentication, authorization, and environment scoping in reusable hooks
or services rather than duplicating checks in handlers.

Database access belongs in repositories and uses parameterized SQL plus deterministic migrations.
Services own multi-step business transactions and relationship assembly; route handlers do not
issue SQL. Cursor pagination uses stable ordering and must not expose secrets in cursors or list
projections.

## C++23 and Fiber Conventions

Native development follows the relevant `fiber-gateway-cpp` standards in addition to the product
rules in this file. The supported native environment is Linux, CMake 4.1 or later, Clang 17 or
later or GCC 13 or later, and C++23.

### Structure and style

- Use four-space indentation and same-line opening braces.
- Use namespaces under `fiber::access_server` or the relevant reusable `fiber::...` module.
- Use `PascalCase` for classes and types. Follow the existing snake_case convention for functions,
  methods, and variables.
- Use `FIBER_ACCESS_SERVER_<NAME>_H` header guards for access-server headers.
- Keep includes explicit and headers focused. Include reusable public headers through
  namespaced `<fiber/...>` paths. Do not expose upstream private `src/` paths or add legacy include
  roots.
- Keep `.cpp` implementations and tests with their logical module. Do not combine unrelated
  config, routing, execution, runtime, and observability responsibilities.
- Format changed native C/C++ files with clang-format 17 or later using
  `third_party/fiber-gateway-cpp/.clang-format`. Format once after the implementation stabilizes and
  inspect the diff. The submodule's `format_code.sh` resolves the submodule Git root and therefore
  does not format sibling files under `native/access-server`.

### Correctness and error handling

- Establish required invariants at construction or initialization boundaries. Minimize nullable
  steady-state members and repeated defensive null checks in request hot paths.
- Do not use C++ exceptions and do not write `throw`. Propagate expected failure through the
  existing result/status types. Use `noexcept` for callbacks, shutdown paths, destructors, and other
  contracts that cannot recover by throwing.
- Use `fiber::event::EventLoop::current().now()` for time in EventLoop request paths so time remains
  consistent with loop scheduling and tests.
- Never block an EventLoop on rnacos, CAT, DNS, database work, logging, upstream HTTP, process
  waits, or cross-thread synchronization.
- Parse JSON and compile expressions, templates, headers, CIDRs, and routing data before publishing
  a candidate snapshot. Requests must never observe a partially built model.
- Preserve immutable compiled snapshots, request pinning, explicit EventLoop ownership,
  backpressure, cancellation, connection-pool lease lifetimes, and ordered shutdown.
- Keep callbacks and referenced objects on their owner loop. Make lifetime, cancellation, and
  shutdown ordering explicit whenever asynchronous work crosses components.

### Performance

The data plane is performance-first. Measure or reason explicitly about hot-path allocations,
copies, contention, and callback shape.

- Minimize allocation and release churn in routing, proxying, streaming, metrics, tracing, and
  logging paths.
- Do not introduce repeated JSON materialization or allocation-heavy `std::string`, `std::vector`,
  `std::function`, shared ownership, or copies in a hot path without justification.
- Prefer views, reusable buffers, fixed-size structures, immutable snapshots, intrusive or custom
  ownership, and compile-time callables when their lifetimes are safe.
- Keep metrics labels bounded. Do not turn arbitrary projects, routes, clusters, hosts, headers, or
  user data into unbounded time series.
- Logging and observability must not apply reverse pressure to request processing. Preserve bounded
  queues and explicit overload behavior.

### CMake and dependencies

- Keep application and test source lists explicit; do not glob native source files.
- Consume reusable code through exported CMake targets and public includes. Do not copy framework
  implementation into access-server.
- Fail configuration clearly when the submodule is missing. Keep generated dependency state in the
  build tree, never in the submodule.
- Preserve compatible IPO/LTO settings across Fiber and access-server targets.
- Do not add a dependency to a request hot path without reviewing ownership, allocation,
  cancellation, binary-size, and build implications.

## Testing and Compatibility

- Native tests use GoogleTest/CTest. Put them under `native/access-server/tests/`, name files
  `*Test.cpp`, and register every source explicitly in `native/access-server/CMakeLists.txt`.
- Add focused native tests before or with behavior changes. Cover valid and invalid candidates,
  failure-old-snapshot behavior, request pinning, cancellation, shutdown, and loop ownership where
  applicable.
- Preserve golden coverage for accepted and rejected Java wire inputs, Host/Path selection,
  RESPONSE/PROXY behavior, WebSocket tunneling, hot updates, gray/service selection, trace
  propagation, and stable error results.
- Native unit tests must be deterministic and use fakes or loopback servers. They must not require
  live rnacos, CAT, public DNS, production configuration, or public upstream services.
- Use Node's test runner and Fastify injection for backend tests unless the repository deliberately
  adopts another runner. Cover validation, authorization, environment isolation, immutable
  releases, partial rnacos publication, retries/idempotency, secret redaction, and rollback as a new
  release.
- Add frontend test tooling before relying on complex editor or workflow state without automated
  coverage. Also verify keyboard access, error focus, unsaved-change guards, responsive layouts,
  and unambiguous draft/published/activation labels.
- End-to-end tests use disposable local services and deterministic upstreams. Production
  credentials and live environments must never be test prerequisites.

Run validation in proportion to the changed scope:

| Changed scope                                                                              | Required minimum validation                                                               |
| ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------- |
| `web/`, `server/`, or shared TypeScript config                                             | `npm run typecheck`, `npm test`, `npm run format:check`, `npm run build`                  |
| `native/access-server/`                                                                    | native configure/build, focused CTest label, clang-format diff review                     |
| Fiber gitlink, native CMake, codec, routing, proxy state, EventLoop ownership, or shutdown | full applicable Fiber/native test suite plus focused access-server tests                  |
| Wire/config behavior shared with Console                                                   | both console and native matrices, plus fixtures/docs                                      |
| Deployment                                                                                 | render/validate deployment config and run targeted health checks without expanded secrets |

Document any test that cannot be run and why. Do not claim production compatibility from unit tests
alone when a documented differential or cutover gate is still outstanding.

## Commit and Pull Request Guidelines

Use Conventional Commits in the form `type(scope): subject`. Preferred types are `feat`, `fix`,
`refactor`, `perf`, `test`, `build`, `docs`, and `chore`. Useful scopes include `web`, `server`,
`access-server`, `config`, `routing`, `rnacos`, `observability`, `build`, and `docker`. Add a
`BREAKING CHANGE:` footer when required.

Keep commits focused. Pull requests must describe user-visible, wire-contract, performance, data
model, security, and operational changes and list the exact validation commands run. Include
before/after screenshots for visual changes. Highlight schema migrations, secret handling, rnacos
publication semantics, activation evidence, hot-path allocation, EventLoop ownership, shutdown
behavior, dependency pin changes, and local compatibility patches.
