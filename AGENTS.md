# Repository Guidelines

## Project Purpose

This repository is the complete Access Gateway product. It is intended to own both:

- the C++23 `access-server` data plane migrated from
  `fiber-net-gateway/fiber-gateway-cpp/apps/access-server`; and
- the management console, including its web frontend and backend API.

The data plane selects projects and routes, applies Host/Path/entry/CIDR/gray policies, serves
configured responses, proxies HTTP and WebSocket traffic, consumes rnacos configuration and
NamingService data, and emits metrics, traces, and structured access logs. The console manages the
configuration and release workflow around that runtime. It must not become a second traffic proxy
or a generic rnacos console.

Keep configuration lifecycle states distinct in storage, APIs, metrics, and UI: a draft saved by
the console, content published to rnacos, and content proven active on a particular access-server
instance. A successful rnacos write or readback is not activation evidence. Until the data plane
exposes typed per-instance evidence, display activation as unknown rather than inferring success.

## Current Bootstrap State

The repository currently contains only the initial README, license, and C++-oriented `.gitignore`.
The directory layout and commands below are the target repository contract and become executable as
their manifests are added. Do not claim that a build or test command passed when the corresponding
project files do not exist yet.

Bootstrap in coherent vertical steps: establish the pinned native build and migrated test target,
then the independently testable console API and web shell, then persistence/publication workflows,
and finally the reproducible end-to-end deployment. Keep the repository runnable at the end of each
step.

## Source Ownership and Upstream Boundaries

- Put repository-owned data-plane code under `native/access-server/`. The initial import must copy
  the complete upstream application, including its `src/`, `tests/`, `docs/`, fixtures, scripts,
  example environment file, and CMake target definitions.
- Record the exact source revision in `native/access-server/UPSTREAM.md` and preserve the applicable
  upstream Apache-2.0 license in `native/access-server/LICENSE.upstream`. The upstream `master`
  revision observed during repository initialization was
  `0fda7764bf94944aca4b674ab5ab311184703118`; verify and deliberately pin the import revision when
  performing the migration rather than silently following a moving branch.
- After import, `native/access-server/` is maintained here. Implement product fixes here. Do not
  build or import the upstream `apps/access-server` copy as a second application target.
- Keep the reusable Fiber runtime, HTTP, script/JSON, Nacos, CAT, and Prometheus modules as a pinned
  `fiber-gateway-cpp` CMake dependency. Pin both the revision and source archive SHA-256. Updating
  that runtime pin is separate from updating the historical application import and requires native
  regression tests.
- If an upstream compatibility patch is unavoidable, keep it narrow under `native/patches/`, tie it
  to the pinned revision, document the upstream Issue or PR, and remove it once the dependency pin
  includes the fix.
- An ignored `.temp/fiber-gateway-cpp/` checkout may be used for source research only. Never import
  from it at build time, develop the product in it, or commit it.
- Preserve the Java compatibility baseline already documented by upstream access-server:
  `ploto-gateway` commit `22c2bf543b96b52c0ccecd4ceb07d4911c502f45`. If that baseline changes,
  update compatibility docs, fixtures, and differential records before changing behavior.

## Planned Project Structure

Use this top-level organization unless a committed design decision supersedes it:

- `web/`: React, TypeScript, and Vite management frontend. Put reusable UI in
  `web/src/components/`, route-level screens in `web/src/pages/`, and typed API access in
  `web/src/api/`.
- `server/`: Node.js, TypeScript, and Fastify control-plane API. Keep application construction
  independent of process startup so tests can use Fastify injection without opening sockets.
- `server/src/config/`: validated environment parsing; do not scatter `process.env` reads.
- `server/src/database/`: database connection management and deterministic migrations.
- `server/src/modules/<domain>/`: domain routes, schemas, services, repositories, and colocated
  tests. Expected domains include environments, projects/routes, gray rules, releases, users, and
  publication evidence.
- `native/CMakeLists.txt`: top-level native build and pinned Fiber dependency.
- `native/access-server/src/`: migrated modules grouped as upstream does: `config/`, `routing/`,
  `execution/`, `runtime/`, and `observability/`.
- `native/access-server/tests/`: GoogleTest coverage and Java compatibility fixtures.
- `deploy/`, root container files, and `compose.yaml`: the reproducible end-to-end stack.
- `docs/`: cross-component architecture, release, deployment, and operational decisions that do
  not belong to one component.

Generated or private paths such as `node_modules/`, all `dist/` directories, `native/build*/`,
`.temp/`, local database data, and environment/credential files must remain ignored and must not be
edited or committed.

## Architecture and Configuration Boundaries

- `access-server` is the only component that receives and proxies gateway traffic. The console API
  manages configuration and operations; it must not forward ordinary gateway requests.
- The control plane must not reach into data-plane memory or make access-server read console
  database tables. Runtime configuration crosses the boundary through the established rnacos
  wire contract. Operational status should use explicit, authenticated, bounded APIs or service
  discovery when those capabilities are added.
- Keep database, rnacos, and access-server clients behind typed services. HTTP route handlers must
  not contain SQL, rnacos protocol details, or ad hoc runtime calls.
- Unit-test construction must not require MySQL, rnacos, CAT, upstream services, public networks,
  or wall-clock timing. Open external connections only in explicit lifecycle steps and close them
  in reverse dependency order.
- Treat project/route publication as a multi-resource workflow, not a transaction that rnacos does
  not provide. Persist an immutable release before publication and record each Data ID write,
  readback, failure, and later instance evidence independently. Rollback creates a new release from
  historical content; it never rewrites release history.
- Validate console configuration in stages: field syntax, relationships across resources, then the
  complete environment. The repository-owned native codec and compiled route model are the source
  of truth for accepted fields, defaults, scalar coercions, semantic validation, and failure-old
  snapshot behavior.
- Never bypass `access-server` version semantics. In particular, route candidates with the same
  project version are ignored, invalid candidates retain the prior snapshot, and removing a
  project or Host mapping has different semantics from a failed update.

Preserve these default rnacos contracts unless an explicit compatibility change is documented and
tested:

| Purpose | Data ID | Group |
| --- | --- | --- |
| Project list | `ploto.unified-access.projects` | `ACCESS-SERVER` |
| Project route | `ploto.unified-access.route.<project>` | `ACCESS-SERVER` |
| Production gray rules | `ploto.unified-access.gray-match` | `DEFAULT_GROUP` by default |

The project list is a semicolon-separated string, not JSON. Per-project route data and gray rules
must retain the native compatibility codec's wire behavior. A normalized console model may be
richer internally, but publication must compile it deterministically into this established format.

Nacos credentials, database passwords, session and signing secrets, CAT configuration secrets,
authorization headers, cookies, request/response bodies, and sensitive route header values must
never be logged, committed, included in plaintext diffs, or returned after one-time secret
delivery. Redact secrets in API responses and operational diagnostics.

## Build, Test, and Development Commands

Run project commands from the repository root. Establish a root npm workspace so the intended
control-plane command surface is:

- `npm install`: install the root lockfile and the `web`/`server` workspaces.
- `npm run dev`: start Vite and Fastify in watch mode.
- `npm run dev:web` and `npm run dev:server`: start one control-plane workspace.
- `npm run typecheck`: run strict TypeScript checking in all workspaces.
- `npm test`: run deterministic backend and configured frontend tests.
- `npm run format` and `npm run format:check`: apply or verify repository formatting.
- `npm run build`: build both control-plane workspaces.

Expose root scripts for the native workflow once `native/CMakeLists.txt` exists. They should be
equivalent to:

```bash
cmake -S native -B native/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_BUILD_TESTS=ON
cmake --build native/build --target fiber_app_access_server --parallel
cmake --build native/build --target fiber_access_server_tests --parallel
ctest --test-dir native/build --output-on-failure
```

The migrated executable target remains `fiber_app_access_server`, with output name
`access-server`; the focused test target remains `fiber_access_server_tests`.

Before submitting control-plane or shared configuration changes, run type checking, tests,
format checking, and the production build. Before submitting native changes, build the application
and run the focused native tests. Changes to the Fiber pin, native CMake wiring, configuration
codec, routing, proxy state machine, EventLoop ownership, or shutdown require the full native test
suite. For deployment changes, validate rendered Compose configuration and perform targeted health
checks without printing expanded secrets.

## TypeScript, React, and API Conventions

Use TypeScript with strict checks. Use two-space indentation, single quotes, no semicolons, trailing
commas, and a 100-character print width once Prettier is configured. Use `PascalCase` for React
components and types, `camelCase` for functions and variables, and kebab-case for CSS classes.

Use functional React components and data-driven rendering for repeated UI. Preserve accessible
labels, keyboard operation, responsive layouts, reduced-motion behavior, and unsaved-change
guards. Do not use color as the only distinction between environments, draft/published state,
failures, or activation status.

Backend routes live under `/api`. Define explicit TypeScript types and Fastify schemas for request,
response, path, and query data. Return stable machine-readable error codes and field paths for
validation errors. Keep authorization and environment scoping in reusable hooks/services rather
than duplicating checks in handlers.

Database access belongs in repositories and uses parameterized SQL plus deterministic migrations.
Services own multi-step business transactions and relationship assembly; route handlers do not
issue SQL. Cursor pagination should use stable ordering and must not expose secrets in cursors or
list projections.

## C++23 and CMake Conventions

Follow the migrated Fiber conventions in `native/access-server/`:

- Use C++23, four-space indentation, braces on the same line, and namespaces under
  `fiber::access_server` or the relevant reusable `fiber::...` module.
- Use PascalCase for classes and types and the existing snake_case convention for functions,
  methods, and variables. Header guards use the `FIBER_ACCESS_SERVER_<NAME>_H` form.
- Keep includes explicit, headers focused, and `.cpp` implementations colocated by module. Include
  reusable Fiber headers through public `<fiber/...>` paths; do not expose the dependency's private
  `src/` tree or add legacy compatibility include roots.
- Do not use C++ exceptions or write `throw`. Propagate expected failures through the existing
  result/status types and use `noexcept` for callback, shutdown, and other non-throwing contracts.
- Establish required invariants at construction/initialization boundaries. Avoid nullable
  steady-state members and repeated defensive null checks in request hot paths.
- Use `fiber::event::EventLoop::current().now()` for time in EventLoop request paths. Never block
  an EventLoop on rnacos, CAT, DNS, database work, logging, or upstream HTTP.
- Preserve immutable compiled snapshots, request pinning, explicit loop ownership, backpressure,
  cancellation, pool lease lifetimes, and ordered shutdown. Parse JSON, compile expressions,
  templates, headers, CIDRs, and routing data before publishing a candidate snapshot.

The data plane is performance-first. Minimize allocation/release churn in routing, proxy,
streaming, metrics, and tracing hot paths. Do not introduce repeated JSON materialization,
allocation-heavy `std::string`/`std::vector`/`std::function`, shared ownership, or copies by default.
Prefer views, reusable buffers, fixed-size structures, immutable snapshots, intrusive/custom
ownership, and compile-time callables where lifetimes permit.

Keep native CMake source lists and dependencies explicit; do not glob application sources or test
files. Do not update the pinned Fiber revision or archive hash without reviewing upstream changes,
updating provenance, and running the native regression matrix.

## Testing and Compatibility Expectations

- Native tests use GoogleTest/CTest. Put tests under `native/access-server/tests/`, name them
  `*Test.cpp`, and register each source explicitly in `native/access-server/CMakeLists.txt`.
- Preserve the upstream compatibility contract and migration documents with the application.
  Golden fixtures must cover accepted and rejected Java wire inputs, Host/Path selection,
  RESPONSE/PROXY behavior, WebSocket tunneling, hot updates, gray/service selection, trace
  propagation, and stable error results.
- Native unit tests must be deterministic and should use fakes or loopback servers. They must not
  require live rnacos, CAT, public DNS, production configuration, or public upstream services.
- Use Node's test runner and Fastify injection for backend tests unless the repository deliberately
  adopts another runner. Test validation, authorization, environment isolation, immutable releases,
  partial rnacos publication, retries/idempotency, secret redaction, and rollback-as-new-release.
- Add frontend test tooling before relying on complex editor or workflow state without automated
  coverage. Until then, manually verify desktop/mobile layouts, keyboard access, error focus,
  unsaved-change guards, and unambiguous draft/published/activation labels.
- End-to-end tests should use disposable local services and deterministic upstreams. Never make
  production credentials or live environments a test prerequisite.

## Commit and Pull Request Guidelines

Use Conventional Commits in the form `type(scope): subject`. Preferred types are `feat`, `fix`,
`refactor`, `perf`, `test`, `build`, `docs`, and `chore`; useful scopes include `web`, `server`,
`access-server`, `config`, `routing`, `rnacos`, `observability`, `build`, and `docker`. Add a
`BREAKING CHANGE:` footer when needed.

Keep commits focused. Pull requests should describe user-visible, wire-contract, performance, data
model, security, and operational changes and list the exact validation commands run. Include
before/after screenshots for visual changes. Highlight schema migrations, secret handling, rnacos
publication semantics, activation evidence, hot-path allocation, shutdown behavior, dependency pin
changes, and local compatibility patches.
