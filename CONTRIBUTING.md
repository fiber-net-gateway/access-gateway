# Contributing to Fibron

Thank you for your interest in contributing to **Fibron** _(formerly Access Gateway)_! Both the
C++23 data plane (`native/access-server/`) and the React/Fastify control plane (`web/`, `server/`)
are actively developed.

## High-Impact Areas Looking for Contributors

If you are looking for a meaningful place to start, these open milestones are especially welcome:

- **OIDC Provider Adapter (`server/`, `web/`)** — enable production SSO login for `AUTH_MODE=oidc`.
- **Standalone File Config Provider (`native/access-server/`)** — run `access-server` from local
  YAML/JSON files without requiring rnacos for small or single-node deployments.
- **Audit Log Viewer UI & API (`server/`, `web/`)** — expose persisted MySQL audit events safely.
- **Gray Rule Editor (`server/`, `web/`)** — add Console authoring & validation for
  `ploto.unified-access.gray-match`.
- **Kubernetes Helm Chart (`deploy/`)** — production-ready manifests for `access-server` + Console.

## Development Setup

### Prerequisites

- **Console:** Node.js `20.19+` with npm workspaces.
- **Native (Linux):** CMake `4.1+`, Ninja, and Clang `17+` or GCC `13+` with C++23 support.

### Clone and Initialize Submodule

```bash
git clone --recurse-submodules https://github.com/fiber-net-gateway/access-gateway.git
cd access-gateway
```

> Do not edit files inside `third_party/fiber-gateway-cpp/` as part of a product PR, and do not run
> `git submodule update --remote`. Reusable Fiber framework improvements should be submitted
> upstream to `fiber-net-gateway/fiber-gateway-cpp`.

### Common Commands

```bash
# Console (web + server)
npm install
npm run typecheck
npm test
npm run format:check
npm run build

# Native data plane
npm run configure:native
npm run build:native
npm run build:native-validator
npm run test:native
```

## Coding & Testing Guidelines

Please read [AGENTS.md](AGENTS.md) for the complete repository conventions. Key expectations:

- **TypeScript / React:** strict TypeScript, Prettier formatting (`npm run format:check`), Fastify
  schema validation, parameterized SQL in repositories, no secrets in logs/diffs/responses.
- **C++23 (`native/access-server/`):** 4-space indentation, no exceptions (`throw`), never block an
  EventLoop, preserve immutable snapshots and fail-closed / failure-old-snapshot guarantees, format
  changed files with `npm run format:native` (`clang-format` 17+).
- **Cross-component changes:** if a change touches the wire contract, defaults, validation, or
  activation evidence, update native code/tests, server schemas/services, frontend types, and docs
  together.
- **Lifecycle honesty:** keep `draft`, `published`, `active`, and `unknown` distinct—never infer
  activation from a config-store write.

## Validation Matrix Before Opening a PR

| Changed Scope                   | Required Commands                                                              |
| ------------------------------- | ------------------------------------------------------------------------------ |
| `web/`, `server/`, root TS/docs | `npm run typecheck && npm test && npm run format:check && npm run build`       |
| `docs/user-guide/`              | `npm run docs:build && npm run docs:check`                                     |
| `native/access-server/`         | `npm run configure:native && npm run test:native` + `clang-format` diff review |
| `compose*.yaml` / `deploy/`     | `docker compose -f compose.yaml config --quiet` and `compose.prebuilt.yaml`    |

## Commit Messages

Use Conventional Commits: `type(scope): subject` (e.g., `feat(server): add oidc provider adapter`,
`fix(access-server): handle empty sni extension`, `docs(readme): add architecture diagram`).
