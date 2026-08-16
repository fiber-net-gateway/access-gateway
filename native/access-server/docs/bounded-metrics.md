# Bounded access-server metrics

`access-server` exposes Prometheus text on its dedicated metrics listener. Every label value is
selected from a compile-time table. Project names, routes, clusters, hosts, Data IDs, MD5 values,
service names, headers, addresses, and request data are never used as metric labels.

The metrics listener is not an instance-activation API and currently has no application-layer
authentication. Bind it to an operational network with deployment-level access controls. The
aggregates documented here do not contain configuration identifiers or secrets.

## Request metrics

The existing worker-sharded metrics are:

- `access_server_requests_total{result}` with `success`, `client_error`, `server_error`, or
  `canceled`;
- `access_server_requests_inflight`;
- `access_server_request_duration_seconds`;
- `access_server_response_compression_total{result}` with `gzip`, `identity`, or
  `not_acceptable`.

Request workers update pre-bound Fiber Prometheus references. No label registration or allocation
occurs in the request path.

## Configuration update metrics

`access_server_config_updates_total{resource,result,reason}` has only the following series:

| `resource`      | `result`  | `reason`            | Meaning                                      |
| --------------- | --------- | ------------------- | -------------------------------------------- |
| `project_list`  | `success` | `accepted`          | A valid Project List candidate was applied   |
| `project_list`  | `failure` | `subscription`      | The Project List subscription failed/closed  |
| `project_list`  | `failure` | `decode`            | The Project List candidate was invalid       |
| `project_route` | `ignored` | `empty`             | Empty/not-found/null content retained state  |
| `project_route` | `ignored` | `version_unchanged` | The last successful version was retained     |
| `project_route` | `success` | `published`         | A complete Project snapshot was published    |
| `project_route` | `success` | `unloaded`          | Empty Host configuration removed its routes  |
| `project_route` | `success` | `removed`           | Project List reconciliation removed Project  |
| `project_route` | `failure` | `subscription`      | Per-Project subscription failed/closed       |
| `project_route` | `failure` | `decode`            | Native compatibility decoding failed         |
| `project_route` | `failure` | `compile`           | Validation or snapshot compilation failed    |
| `project_route` | `failure` | `service_ready`     | NamingService readiness failed               |
| `project_route` | `failure` | `publish`           | Global snapshot construction failed          |

Retries count each failed subscription attempt. Stale compiler results and candidates canceled by
a newer generation are not update outcomes and do not increment a series.

## Readiness and snapshot metrics

`access_server_config_readiness{state}` is one-hot across these fixed states:

- `waiting_for_project_list`;
- `synchronizing_projects`;
- `ready`;
- `unavailable`;
- `stopped`.

`access_server_config_projects{state}` reports aggregate counts for `desired`, `subscribed`,
`synchronized`, `retrying`, `processing`, and `rejected`. A readiness value of `ready` means every
desired Project reached a terminal first result; rejected Projects remain visible in the separate
count. It is not proof that every candidate was published or that a particular instance serves a
control-plane release.

The active route model is described by:

- `access_server_route_snapshot_resources{resource}` for `project`, `host`, `route`, and
  `compiled_program`;
- `access_server_route_snapshot_generation`;
- `access_server_route_snapshot_age_seconds`;
- `access_server_route_snapshot_estimated_bytes`;
- `access_server_route_snapshot_static_response_bytes`.

Generation is process-local, starts at zero, and advances only when the watcher publishes a new
global snapshot. Failed, ignored, or stale candidates do not advance it. It resets on process
restart and is not a rnacos release number or activation token.

## Nacos component lifecycle and service discovery

`access_server_nacos_component_lifecycle{component,state}` is one-hot for `client`,
`config_service`, and `naming_service`. The fixed lifecycle states are `created`, `starting`,
`running`, `failed`, `stopping`, and `stopped`.

This metric reports only access-server's calls into each component and their immediate start
result. In particular, `running` means that `start()` succeeded; it does **not** mean that a Nacos
transport is connected, authenticated, or currently able to reconnect. Fiber does not yet expose
that evidence through its public API. The required upstream API is tracked by
[fiber-gateway-cpp #27](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/27). Until a
pinned Fiber revision provides it, access-server intentionally emits no `connected` metric.

`access_server_discovery_events_total{operation,result,reason}` uses this fixed matrix:

| `operation` | `result` | `reason`                                                                                  |
| ----------- | -------- | ----------------------------------------------------------------------------------------- |
| `update`    | `success` | `changed`                                                                                |
| `update`    | `ignored` | `unchanged`                                                                              |
| `retire`    | `retired` | `released`, `subscription_closed`, or `shutdown`                                         |
| `acquire`   | `success` | `acquired`                                                                               |
| `acquire`   | `failure` | `invalid_argument`, `shutdown`, `authentication_unavailable`, `transport`, `grpc_status`, |
|             |           | `protocol`, `server`, or `response_too_large`                                             |

Acquire failures are mapped directly from Fiber's typed `NamingServiceErrorCode`; the service
name and diagnostic message are not retained by metrics.

`access_server_discovery_resources{resource}` exposes four fixed aggregates:

- `ready_service`: initialized ServiceDiscovery states that received a first snapshot;
- `selectable_endpoint`: healthy, enabled, positive-weight endpoint definitions accepted by those
  states after identity deduplication;
- `logical_cluster`: the sum of compiled logical clusters across ready service states;
- `selector_lease`: live route selector objects holding a ServiceDiscovery lease.

Endpoint and cluster values are sums across service states, not counts of globally unique names.
Pending subscriptions are deliberately not inferred: the pinned Fiber API does not notify the
application when an entry retires before its first snapshot, so only ready state is reported as an
exact aggregate.

## TLS certificate rotation and reclamation

Dynamic downstream TLS identities use worker hazard pointers so a ClientHello selection does not
take shared ownership of the complete certificate snapshot. The following metrics describe the
bounded lifecycle of those snapshots:

- `access_server_tls_certificate_rotations_total` advances when a publication replaces an active
  snapshot; the initial publication is not a rotation;
- `access_server_tls_certificate_reclaim_runs_total{trigger}` counts scans for the fixed triggers
  `publish`, `hazard_clear`, and `shutdown`;
- `access_server_tls_certificate_reclaimed_snapshots_total{trigger}` counts snapshot objects freed
  by those scans;
- `access_server_tls_certificate_retired_snapshots` is the current number waiting for worker
  hazards to clear;
- `access_server_tls_certificate_oldest_retired_age_seconds` is zero when none are retained;
- `access_server_tls_certificate_max_retention_seconds` is the longest completed or currently
  ongoing retention observed by this process.

The duration metrics use the Nacos owner EventLoop's monotonic clock and reset on process restart.
They are not certificate-validity ages. Certificate IDs, DNS names, SNI values, PEM content, Data
IDs, versions, and digests are not retained by these metrics.

A routine handshake clears its worker hazard and reads one `retirement_pending` atomic. When no
rotation has left a retired snapshot, it does not post to the Nacos loop and does not increment a
reclaim series. Publication performs the first scan synchronously; a `hazard_clear` scan is eligible
only during the short interval in which a real retirement is pending.

## Concurrency and cost

The Nacos owner EventLoop is the sole writer for configuration snapshots, Nacos component
lifecycle, service aggregates, and TLS retirement aggregates. Events use fixed atomic arrays.
Configuration, discovery, and TLS aggregates use sequence-checked groups of atomics so metrics
workers take lock-free coherent samples. Service selector destruction may occur on a request worker
and updates only one relaxed atomic lease counter; it never posts, blocks, or calls back into
ServiceDiscovery. TLS hazard clear posts only while a retirement is pending.

Route counts and byte totals are cached during the global snapshot's existing build traversal, so
the configuration observer update is O(1). Discovery endpoint and cluster totals are derived from
the vectors already constructed for selection and updated only when a NamingService snapshot
changes; no extra traversal occurs on scrape or request execution. Rendering and output buffer
allocation happen only on a Prometheus scrape.

The Fiber registry continues to own request metric shards and their collection lifecycle. The
application blocks are appended through `AccessRuntimeMetrics` after Fiber's text snapshot; no
cross-EventLoop `CounterRef`/`GaugeRef` mutation is required.

## Remaining scope

The implemented increments cover Project List/route outcomes, route readiness and snapshot
size/age, application-owned Nacos lifecycle, service/endpoint/cluster/selector aggregates, and TLS
rotation/reclamation. Actual Nacos transport/reconnect state remains blocked on Fiber #27.
DNS/pool/proxy/WebSocket outcomes and async logging/CAT drops remain separate O-02 increments.
Typed, authenticated, per-instance activation evidence remains O-01 and must continue to be
reported as unknown until implemented.
