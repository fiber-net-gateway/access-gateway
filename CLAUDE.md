# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Access Gateway is a gateway product with two first-class, actively developed parts:

- `native/access-server/` — the C++23 data plane (migrated from `fiber-gateway-cpp/apps/access-server`), the **only** component on the gateway traffic path.
- `web/` + `server/` — the React/Vite management console and the Node.js/Fastify control-plane API.

The native application is a maintained product copy, not a frozen import: implement data-plane features, fixes, compatibility behavior, performance work, and operational improvements in this repository. The console is not the sole development target.

`AGENTS.md` at the repository root is the authoritative development-conventions document (product scope, component boundaries, TS/C++ conventions, testing expectations, validation matrix, commit guidelines). Read it before non-trivial changes; this file is an orientation summary.

## Architecture (big picture)

The control plane authors configuration, rnacos carries it to the data plane, and the data plane serves traffic. The console must not become a second traffic proxy; the control plane must not access data-plane memory, and access-server must not read console DB tables — the boundary is the rnacos wire contract plus explicit authenticated APIs for operational status.

```
Operator → React console (web) → Fastify API (server) → MySQL
                                  │  publication worker / activation collector (server/src/processes)
                                  ▼
   rnacos (Nacos) ─────────► native access-server ─► upstream / CAT / Prometheus / structured logs
   Nacos NamingService ────► access-server (service discovery)
```

Key invariants (keep these distinct in storage, APIs, metrics, and UI):

- **Lifecycle states**: draft (saved by console) → published (written to rnacos) → active (proven on a specific instance). A successful rnacos write or readback is **not** activation evidence. Until the data plane exposes typed per-instance evidence, report activation as unknown rather than inferring success.
- **Native codec is the source of truth** for fields, defaults, scalar coercion, semantic validation, and failure-old-snapshot behavior. The console validates in stages (field syntax → cross-resource → full environment) and publication deterministically compiles its model into the native wire format.
- Config candidates are fully parsed and compiled before immutable publication; invalid candidates retain the prior active snapshot; same-version candidates are ignored; removing a project/Host mapping differs from a failed update.
- Publication is a multi-resource workflow (persist an immutable release first, record every Data ID write/readback/failure/evidence independently), never a rnacos transaction. Rollback creates a new release from historical content; it never rewrites release history.

### Native (`native/access-server/`)

- `src/` groups application modules by responsibility: `config/` (codecs/coercion/limits), `routing/` (compiled snapshots, matchers, CIDR, gzip), `execution/` (RESPONSE/PROXY executors, request handler), `runtime/` (server, supervisors, watchers), `observability/` (metrics, traces, logs, activation evidence), `validation/` (validator protocol).
- `third_party/fiber-gateway-cpp/` is a pinned, read-only submodule providing reusable Fiber runtime, HTTP, Nacos, CAT, Prometheus, JSON, and script components. Never edit it as part of a product change — contribute reusable fixes upstream, then update the gitlink deliberately (never `git submodule update --remote`). Temporary compatibility patches belong under `native/patches/`, bound to the pinned revision. Import revision is tracked in `native/access-server/UPSTREAM.md`.
- `tests/` holds GoogleTest/CTest coverage: `*Test.cpp` files registered explicitly in `native/access-server/CMakeLists.txt`, plus Java compatibility fixtures under `tests/fixtures/java`. Java wire-compat baseline: `ploto-gateway` commit `22c2bf543b96b52c0ccecd4ceb07d4911c502f45`.
- App target `fiber_app_access_server` (output `access-server`, written to `native/build/apps/access-server`); focused test target `fiber_access_server_tests` (CTest label `access-server`).
- `access-gateway-validator` (target `fiber_app_access_gateway_validator`) is an offline stdin/stdout validator reusing the native codec/compiler — no Nacos or network. The server runs it fail-closed via `NATIVE_VALIDATOR_PATH` and requires its `--describe-config-limits` schema before Release preparation.

### Console (`server/` + `web/`)

- `server/src/app.ts` / `runtime.ts` keep application construction separate from process startup so tests use Fastify injection without opening sockets. `config/` is the only place allowed to read `process.env`. `database/` owns connections and deterministic, checksum-protected migrations (the API never runs them automatically). `modules/<domain>/` holds routes, schemas, services, repositories, and colocated tests (e.g. `drafts`, `versions`, `releases`, `publication`, `activation`, `certificates`, `tls`, `projects`, `environments`, `auth`, `audit`, `system`). `processes/` contains the publication worker, activation collector, and migration entrypoints.
- `web/src/` — React 19 + Vite. Reusable UI in `components/`, route-level screens in `pages/`, typed API access in `api/`. Route config is authored as YAML in CodeMirror editors and compiled to the native JSON wire model in `routes/model.ts`.
- Models are envelope-encrypted (AES-256-GCM) before MySQL storage; the local `DOCUMENT_ENCRYPTION_KEY_BASE64` is dev-only (production needs an external key provider).

## Common commands

Run from the repository root. First-time setup: `git submodule update --init --recursive`, `npm install`, `cp server/.env.example server/.env`.

Console:
- `npm run dev` — Vite (:5173) + Fastify (:3000) watch mode; `npm run dev:web` / `npm run dev:server` for one side. Vite proxies `/api` to Fastify.
- `npm run db:migrate` — apply MySQL migrations (needed for persistent API use; requires MySQL 8.4 with `MYSQL_ENABLED=true` and `DOCUMENT_ENCRYPTION_KEY_BASE64` in `server/.env`).
- `npm run typecheck` — strict TypeScript in all workspaces.
- `npm test` — Node test runner (server), vitest (web), plus user-guide doc build check.
- `npm run format` / `npm run format:check` — Prettier (2-space indent, single quotes, no semicolons, trailing commas, 100-char width).
- `npm run build` — build both console workspaces.

Native (Linux; CMake 4.1+, Ninja, Clang 17+ or GCC 13+, C++23):
- `npm run configure:native` — configure the Release build in `native/build` with tests (`-DFIBER_BUILD_TESTS=ON`).
- `npm run build:native` / `npm run build:native-validator` / `npm run test:native` — build app/validator, then build and run the focused access-server tests.
- `npm run format:native` — clang-format repository-owned C/C++ only (pinned Fiber style; never touches `third_party/`). `format:native:all` formats everything repository-owned.
- Advanced native validation: `benchmark:native`, `test:native:sanitizers`, `verify:native:cutover`, `test:native:interop`.

Demo: `npm run demo:init && npm run demo:up` runs the full Docker Compose stack (console+API, MySQL 8.4, R-Nacos, native access-server). Operate with `demo:ps` / `demo:logs` / `demo:down`; `docker compose down --volumes` only when deliberately deleting demo data.

## Running a single test

- Server: `node --import tsx --test server/src/modules/<domain>/<file>.test.ts` (from root; test files are colocated with modules).
- Web (node runner, model): `node --import tsx --test web/src/routes/model.test.ts`; web (vitest, UI): `npm exec --workspace web -- vitest run src/<file>.ui.test.tsx`.
- Native: build the test target (or run `npm run test:native` once), then filter: `ctest --test-dir native/build -L access-server -R <TestName>` or run the `fiber_access_server_tests` binary directly with `--gtest_filter=<Suite>.<Case>`.

## Validation expectations

Match validation to the changed scope (full matrix in AGENTS.md):
- `web/`/`server/` or shared TS config → `npm run typecheck`, `npm test`, `npm run format:check`, `npm run build`.
- `native/access-server/` → native configure/build, focused CTest label, clang-format diff review.
- Fiber gitlink, native CMake, codec, routing, proxy state, EventLoop ownership, or shutdown → full applicable Fiber/native suite plus focused access-server tests.
- Wire/config behavior shared with the console → both console and native matrices, plus fixtures and docs.
- Deployment → render/validate compose and run targeted health checks.

Document any test that cannot be run and why; do not claim production compatibility from unit tests alone while a documented differential or cutover gate is outstanding.

## rnacos contract

| Purpose | Data ID | Group |
|---|---|---|
| Project list | `ploto.unified-access.projects` | `ACCESS-SERVER` |
| Project route | `ploto.unified-access.route.<project>` | `ACCESS-SERVER` |
| TLS certificate snapshot | `ploto.unified-access.tls-certificates` | `ACCESS-SERVER` |
| Gray rules | `ploto.unified-access.gray-match` | `DEFAULT_GROUP` |

The project list is a semicolon-separated string, not JSON. Route and gray-rule payloads must retain the native compatibility codec's Java wire behavior. Any change to the wire model, defaults, validation, routing behavior, publication semantics, or activation evidence is cross-component work: update the native codec/runtime and tests first (or in the same change), then server schemas/services, frontend types and editors, fixtures, and user docs.

## Commit conventions

Conventional Commits `type(scope): subject` — preferred types `feat`, `fix`, `refactor`, `perf`, `test`, `build`, `docs`, `chore`; useful scopes `web`, `server`, `access-server`, `config`, `routing`, `rnacos`, `observability`, `build`, `docker`. Add `BREAKING CHANGE:` footers when required. PRs must list the exact validation commands run and call out schema migrations, secret handling, rnacos publication semantics, activation evidence, hot-path allocation, EventLoop ownership, shutdown behavior, and dependency pin changes.
