# Configuration compilation

`access-server` compiles route and TLS candidates inline on the Nacos owner EventLoop. The watcher
retains subscription, service-discovery, readiness, version, publication, and compilation ownership
on one loop; there is no dedicated compiler thread and no cross-loop job protocol.

This is repository-owned runtime behavior. It does not require a Fiber library change or a generic
Fiber CPU executor.

## Thread model

```text
Nacos owner loop, per configuration notification
  -> enforce the raw payload limit
  -> advance the per-resource generation
  -> decode, validate, and compile the complete candidate synchronously
  -> bind immutable upstream mTLS identity references
  -> bind NamingService leases where required
  -> transition PreparedProjectUpdate to ReadyProjectUpdate
  -> commit only the Ready type, or retain the previous snapshot on failure
```

Watcher maps, readiness state, `RouteConfigStore`, `TlsCertificateStore`, NamingService leases,
subscriptions, publication counters, and the compiler all live on this one loop. Gray-rule
compilation was already inline here; route and TLS compilation now follow the same model.

## Why inline is safe

Measured on a production instance (172.28.2.111, 19.5 h, 367 projects):

- 371 project compilations, 26.3 ms total, 1.53 ms maximum — noise on a loop whose Nacos heartbeat
  eviction threshold is ~15 s, three orders of magnitude of margin;
- the initial batch worst case (1024 projects × ~71 µs) is ~73 ms of synchronous work, still ≥ 9×
  inside the eviction threshold;
- a dedicated compiler loop previously added a cross-thread job protocol (cancellation flags,
  generation rechecks, no-throw allocation failures) whose complexity bought no measurable latency
  benefit at these magnitudes.

Compiles slower than 50 ms log a `project_compile_slow` / `tls_compile_slow` WARN through the
`access_server.config` logger so a pathological snapshot is observable in production. Configuration
failures log at WARN through the same logger: `project_list_failed`, `project_config_failed` (with
stage/data_id/md5/field/offset/error), permanent `project_subscription_failed`, and
`tls_config_failed` / `tls_subscription_failed`. Transient per-project subscription retry rounds
stay unlogged to bound noise during Nacos outages; they remain visible through the
`retrying_projects` readiness counts. The `project_compile` stage-duration metric continues to
record every compilation.

## Project route compilation

`AccessConfigCompiler` delegates the bounded Project model build to the concrete, CPU-only
`ProjectConfigCompiler` on the Nacos loop:

- route JSON decoding and structural validation;
- Host/Path, method, CIDR, address, header, and relationship compilation;
- condition, template, rewrite, and JavaScript route compilation;
- static response decoding and gzip precompression; dynamic response gzip level publication;
- strict native-only outbound `upstream_tls` validation, sealed custom-CA preparation, and stable
  non-zero transport pool-affinity derivation;
- construction of a complete `ProjectRouteSnapshot` candidate.

The immutable Project snapshot owns each route profile's sealed CA descriptor. On the same loop,
`RouteConfigStore` also resolves an optional Certificate Version UUID against the active TLS
identity directory before freezing the candidate. The bound snapshot owns sealed certificate/key
descriptors, and the identity digest participates in pool affinity. A request pinned to an old
snapshot therefore keeps old trust and client identity material alive across rotation; new and old
connection keys cannot cross profiles. Missing identity, invalid CA/name/profile, or ID reuse with
different content retains the previously published Project snapshot. The offline validator checks
the reference shape but cannot claim that an instance has loaded the referenced TLS resource.

The watcher retains only the latest `ConfigData` pointer for a candidate rejected with
`MissingDependency`. A successful TLS snapshot publication retries that bounded set with forced
compilation. This does not bypass same-version semantics for an already published Route. Other
compile failures are never retried by a TLS update.

Pure compilation represents a service route with an unavailable selector that retains only its
normalized service and cluster metadata. After the candidate returns, `RouteConfigStore` binds
those placeholders on the same loop. The selector factory returns an explicit result; it carries
each `ServiceDiscovery::acquire()` failure directly instead of storing a begin/take error side
channel. Binding is the only stage that calls `ServiceDiscovery::acquire()`, preserving Fiber's
owner-loop contract. Service readiness is then awaited before commit as before.

The move-only publication states and their compatibility semantics are specified in
[`config-publication-typestate.md`](config-publication-typestate.md).

The compiler owns a separate `AccessScriptCompiler`, constructed lazily on the Nacos loop. Its
extension data remains alive for the lifetime of runtime snapshots. `AccessScriptRuntime` is a
stateless execution adapter on request workers and does not own compiler libraries or expose a
compile callback. The offline validator also constructs `AccessScriptCompiler` directly, so route
compilation has no dependency on request execution or Nacos runtime targets.

## Per-notification compile and terminal semantics

Each configuration notification compiles once and settles synchronously to a terminal state before
the next notification is processed; there is no queue and no coalescing. The state machine is
unchanged from the queued model — what changed is that a candidate cannot be superseded between
enqueue and completion:

- a same-version candidate whose snapshot is still the loaded one is accepted as
  `VersionUnchanged` without recompiling;
- a same-version candidate that is no longer confirmably loaded (a replay corner that is
  unreachable in practice because replay paths force compilation) is recompiled once with the
  skip bypassed; a bounded two-pass loop makes this structurally terminal;
- removal, NotFound, subscription replacement, and shutdown gate new compilations at the top of
  the compile: `Running` state, current Project identity, and a non-stopped subscription are
  re-checked before each pass.

`AccessConfigReadiness.processing_projects` still reports in-flight candidates during the
synchronous compile window. A healthy instance does not flap to unavailable during a hot update:
`defer_readiness_updates_` suppresses intermediate readiness publications from nested
reconcile/batch stacks, and batch commits remain atomic.

## TLS compilation

TLS compiles inline with the same per-notification semantics:

- strict TLS snapshot JSON decoding;
- version/digest preclassification against the current store state;
- PEM and private-key validation;
- DNS SAN extraction, normalization, duplicate detection, and selector index construction;
- TCP and optional QUIC `TlsContext` creation;
- first-snapshot sealed bootstrap identity preparation.

The loop reclassifies version and digest before commit, then publishes the already prepared
snapshot through the existing hazard-pointer store. An older version, identical same version, and
conflicting same version retain their prior meanings. Invalid or stale work never replaces the
active TLS identity.

`TlsCertificateWatcher::subscribe_processing()` exposes compilation activity without treating a
Nacos write as instance activation evidence. Because compilation is inline, the processing `Watch`
only ever settles to `false` between loop turns: awaiters resumed between turns never observe the
in-stack `true`, so consumers must treat `true` as evidence that a compile is running, not as a
stable state to wait on. Activation evidence still records Processing → terminal transitions
because `publish_evidence()` is called before and after the compile.

## Lifecycle

Ordered shutdown is:

1. stop request serving;
2. close configuration subscriptions and advance all generations;
3. asynchronously join the remaining service-readiness and subscription-retry coroutines
   (`await_ready_project` / `retry_project_subscription`);
4. clear route snapshots and drain TLS hazards;
5. stop Nacos services.

Watcher destructors assert that no background task remains. Inline compilation needs no compiler
thread lifecycle: a compile cannot outlive the stack that started it, and the `Running`-state gate
at each compile entry prevents new compilations once shutdown begins.

Project List parsing and gray-rule parsing also remain on this loop. They are linear, have small
versioned payload/cardinality limits, and do not compile scripts, compression, certificates, or
network resources.
