# Bounded access-server metrics

`access-server` exposes Prometheus text on its dedicated metrics listener. Every label value is
selected from a compile-time table. Project names, routes, clusters, hosts, Data IDs, MD5 values,
service names, headers, addresses, and request data are never used as metric labels.

The `/metrics` route has no application-layer authentication. Bind the status listener to an
operational network with deployment-level access controls. The same listener can expose the
separately authenticated `/v1/activation-evidence` route, but enabling that route does not protect
`/metrics`. The aggregates documented here do not contain configuration identifiers or secrets.

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

## Proxy transport and WebSocket metrics

Proxy execution uses only worker-local, pre-bound counters and gauges. The following terminal
metrics are mutually exclusive within their respective scope:

- `access_server_proxy_executions_total{result}` uses `completed`, `failed`, and `canceled`.
  `completed` means the proxy coroutine returned successfully, including a fully relayed upstream
  HTTP error response; `failed` means it returned an application or I/O error; `canceled` means the
  downstream response channel closed first.
- `access_server_proxy_attempts_total{result}` uses `completed`, `failed`, and `aborted`. An attempt
  begins only after a valid upstream selection. `completed` means its upstream response was fully
  handled, `failed` identifies an upstream-side failure, and `aborted` covers local policy,
  downstream failure, or coroutine cancellation. `access_server_proxy_attempts_inflight` is
  balanced by a cancellation-safe scope.

`access_server_proxy_failures_total{phase}` is an event counter rather than a terminal outcome. A
single execution can report a primary failure and a secondary cleanup failure. Its compile-time
phase set is:

- `no_upstream_hosts`, `upstream_circuit_open`, `invalid_selection`, and `evaluate_context`;
- `resolve_upstream`, `pool_shutdown`, `connect`, and `tls`;
- `build_request`, `build_headers`, `send_header`, `read_request_body`,
  `request_body_too_large`, and `send_request_body`;
- `read_response_header`, `build_response_headers`, `response_body_too_large`,
  `switch_websocket`, `send_response_header`, `read_response_body`, and
  `write_response_body`.

Connection acquisition carries a fixed identifier-free observation back to the request telemetry
owner. It feeds:

- `access_server_proxy_pool_acquires_total{result}` with `hit`, `miss`, or `shutdown` for every
  lease acquisition, including another acquisition before a later resolved address is tried;
- `access_server_proxy_dns_resolutions_total{result}` with `success`, `empty`, `failure`, or
  `unavailable`; literal IP targets and reusable pool hits do not perform or increment DNS;
- `access_server_proxy_connect_attempts_total{result}` with `success`, `failure`, `tls_failure`, or
  `create_failure` for new transport construction and connection attempts.

WebSocket routing exposes `access_server_websocket_handshakes_total{result}` with `accepted`,
`rejected`, or `failed`, plus `access_server_websocket_sessions_total{result}` with `closed` or
`aborted` and `access_server_websocket_sessions_inflight`. A session becomes active only after the
downstream upgrade header is sent. `closed` means Fiber's bidirectional relay returned; `aborted`
means the surrounding proxy coroutine was canceled after acceptance. The pinned relay API returns
no typed peer/timeout/error cause, so access-server does not infer a more specific close reason.

Project, route, cluster, Host, upstream address, DNS name, error message, header, and request data
are absent from every label. Recording performs direct integer increments and copies one bounded
POD observation; it adds no registration, string construction, lock, atomic shared ownership, or
cross-loop post to the request path.

## Asynchronous logging and CAT pipeline metrics

`AccessProcessMetrics` reads the snapshots already maintained atomically by Fiber. The logging
source and primary appender ID are passed explicitly from process startup; the appender ID is the
value returned by `LogConfigBuilder`, not an assumed numeric constant. The logging schema is:

- `access_server_log_metrics_available`, which is `1` only while the injected logger is running
  and its primary appender ID is present;
- `access_server_log_queue_records`, `access_server_log_queue_bytes`,
  `access_server_log_queue_peak_records`, `access_server_log_queue_peak_bytes`, and
  `access_server_log_queue_accepting`;
- `access_server_log_queue_failures_total{reason}` with `queue_full`, `allocation`, or
  `formatting`;
- `access_server_log_appender_records_total{result}` with `written` or `dropped`, plus
  `access_server_log_appender_written_bytes_total`;
- `access_server_log_appender_failures_total{operation}` with `write`, `reopen`, `rotation`, or
  `retention`, plus `access_server_log_appender_rotations_total` and
  `access_server_log_appender_active_file_bytes`.

The appender series describe the one process-owned primary sink. They intentionally have no
appender-name or path label. A console appender reports zero for file-only rotation and active-size
series; keeping the series stable allows a future reviewed file-appender configuration without a
dashboard schema change.

CAT exposes a fixed one-hot `access_server_cat_state{state}` across `disabled`, `created`,
`running`, `stopping`, and `stopped`. A missing CAT configuration is `disabled`, not an apparently
healthy collection of zero counters. Backlog and delivery series are:

- `access_server_cat_queue_messages{kind}` and `access_server_cat_queue_bytes{kind}`, where `all`
  includes every queued frame and `system` is the bounded system-priority subset;
- `access_server_cat_messages_total{result}` with `submitted` or `sent`, and
  `access_server_cat_sent_bytes_total`;
- `access_server_cat_dropped_events_total{reason}` with the compile-time reasons `queue_full`,
  `unavailable`, `sampled`, `partial_frame`, `encode`, `aggregation_overflow`,
  `aggregate_dropped`, `aggregate_retry`, `aggregate_encode`, `metric_overflow`,
  `metric_dropped`, `metric_retry`, `heartbeat_dropped`, `heartbeat_encode`, and
  `heartbeat_provider`.

Some CAT internal counters describe different stages of the same failed delivery. For example, an
aggregate submission failure can also increment a queue loss. The reason series are independently
actionable events and must not be summed as a count of unique messages. CAT app keys, hostnames,
collector/router addresses, message types, trace IDs, and request values are never retained or
rendered by this metric domain.

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

## DNS resolver configuration and health

HTTP worker DNS uses one immutable, prevalidated configuration. These fixed-schema metrics expose
its operational state without retaining nameserver addresses or queried names:

- `access_server_dns_config{source}` is one-hot for `system` or `override`;
- `access_server_dns_nameservers` is the configured count, bounded to three;
- `access_server_dns_resolver_state{state}` is one-hot for `stopped`, `starting`, `ready`, `failed`,
  or `stopping`;
- `access_server_dns_resolvers_active` counts initialized loop-affine worker resolvers;
- `access_server_dns_initializations_total{result}` uses only `success` and `failure`;
- `access_server_dns_config_unsupported{feature}` reports the fixed `search`, `ndots`, `sortlist`,
  `option`, and `directive` flags retained from a system resolver file but not executed.

The worker-sharded `access_server_proxy_dns_resolutions_total{result}` remains the query health
counter. A reusable pool hit or IP-literal upstream does not increment it. Neither metric family
contains a DNS query, nameserver, route, Project, service, or endpoint label.

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
cross-EventLoop `CounterRef`/`GaugeRef` mutation is required. Logging and CAT stats are read only
while rendering a scrape. Those APIs load Fiber-owned atomics; they do not lock, block their writer
threads, post to an EventLoop, or modify the request/log/CAT submission paths. Shutdown closes the
metrics listener and drains outstanding collections before detaching workers and stopping CAT;
the process logger remains alive until the runtime and EventLoops have been destroyed.

## Remaining scope

The implemented increments cover Project List/route outcomes, route readiness and snapshot
size/age, bounded DNS configuration/health, application-owned Nacos lifecycle,
service/endpoint/cluster/selector aggregates, TLS rotation/reclamation, worker-sharded
proxy/DNS/pool/WebSocket outcomes, and async logging/CAT backlog and loss. Actual Nacos
transport/reconnect state is the only remaining O-02 gap. Typed, authenticated, per-instance activation evidence is implemented
separately from these metrics; see
[`../../../docs/activation-evidence.md`](../../../docs/activation-evidence.md).
