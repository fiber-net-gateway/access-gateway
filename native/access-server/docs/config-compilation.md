# Configuration compilation worker

`access-server` compiles route and TLS candidates on one dedicated compiler EventLoop. The Nacos
owner EventLoop retains subscription, service-discovery, readiness, version, and publication
ownership; it does not perform unbounded JSON, script, compression, PEM, or OpenSSL work.

This is repository-owned runtime behavior. It does not require a Fiber library change or a generic
Fiber CPU executor.

## Thread model

```text
Nacos owner loop
  -> enforce the raw payload limit
  -> advance the per-resource generation
  -> retain the immutable ConfigData shared_ptr
  -> enqueue or replace the latest pending candidate

compiler loop
  -> decode and validate
  -> compile a complete route or TLS candidate
  -> post the result to the Nacos owner loop

Nacos owner loop
  -> reject stale generation results
  -> bind NamingService leases where required
  -> wait for service readiness
  -> atomically publish, or retain the previous snapshot on failure
```

Only immutable Nacos data and a job result cross loops. Watcher maps, readiness state,
`RouteConfigStore`, `TlsCertificateStore`, NamingService leases, subscriptions, and publication
counters remain Nacos-loop-only. Cross-loop work uses intrusive `EventLoop::NotifyEntry` callbacks;
the Nacos loop never waits on a future, mutex, condition variable, or worker thread.

## Project route compilation

`AccessConfigCompiler` performs these operations on the compiler loop:

- route JSON decoding and structural validation;
- Host/Path, method, CIDR, address, header, and relationship compilation;
- condition, template, rewrite, and JavaScript route compilation;
- static response decoding and gzip precompression;
- construction of a complete `ProjectRouteSnapshot` candidate.

Pure compilation represents a service route with an unavailable selector that retains only its
normalized service and cluster metadata. After the candidate returns, `RouteConfigStore` replaces
those placeholders on the Nacos loop. This is the only stage that calls
`ServiceDiscovery::acquire()`, preserving Fiber's owner-loop contract. Service readiness is then
awaited before commit as before.

The compiler owns a separate `AccessScriptRuntime`, constructed lazily on the compiler loop. Its
extension data remains alive for the lifetime of runtime snapshots; request workers only execute
the resulting immutable programs.

## Bounded queue and replacement

The compiler EventLoop has at most one posted Project compilation job. Every current Project has at
most one retained latest pending `ConfigData` pointer and one queue entry. The queue is therefore
bounded by the versioned Project List limit of 1024 entries. It retains Nacos' shared immutable data
rather than copying route payloads of up to 4 MiB.

For a new value of the same Project:

1. the generation advances;
2. an older queued value is replaced by the latest pointer;
3. an older posted job is atomically marked canceled;
4. a job that has not started exits without decoding;
5. a job already compiling may finish, but its result is discarded on the Nacos loop;
6. only a result whose Project identity and generation are still current may bind services or
   publish.

FIFO queue order prevents one Project from starving the others. Project removal, NotFound,
subscription replacement, and shutdown remove pending work and invalidate active results.
`AccessConfigReadiness.processing_projects` reports asynchronous candidates separately from the
existing first-synchronization readiness state, so a healthy instance does not flap to unavailable
during a hot update.

## TLS compilation

TLS uses the same compiler loop with one active job and one latest pending pointer. The worker does:

- strict TLS snapshot JSON decoding;
- version/digest preclassification against an owner-loop state copy;
- PEM and private-key validation;
- DNS SAN extraction, normalization, duplicate detection, and selector index construction;
- TCP and optional QUIC `TlsContext` creation;
- first-snapshot sealed bootstrap identity preparation.

The Nacos loop reclassifies version and digest before commit, then publishes the already prepared
snapshot through the existing hazard-pointer store. An older version, identical same version, and
conflicting same version retain their prior meanings. Invalid or stale work never replaces the
active TLS identity. `TlsCertificateWatcher::subscribe_processing()` exposes compilation activity
without treating a Nacos write or compiler completion as instance activation evidence.

## Lifecycle

The process starts the compiler EventLoop group before starting Nacos subscriptions. Ordered
shutdown is:

1. stop request serving;
2. close configuration subscriptions and advance all generations;
3. cancel pending compiler work and asynchronously join every posted result callback;
4. clear route snapshots and drain TLS hazards;
5. stop Nacos services;
6. stop and join the compiler EventLoop group.

Watcher destructors assert that no queued data, active job, or background compiler task remains.
The compiler group must stay running until watcher shutdown completes, including startup rollback.

Project List parsing and gray-rule parsing remain on the Nacos loop. Both are linear, have small
versioned payload/cardinality limits, and do not compile scripts, compression, certificates, or
network resources. Moving them would add cross-loop state without removing comparable CPU work.
