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

## Concurrency and cost

The Nacos owner EventLoop is the sole writer. It records update events in a fixed atomic array and
publishes readiness/snapshot aggregates through a sequence-checked group of atomics. Metrics
workers take a lock-free coherent sample. Route counts and byte totals are cached during the
global snapshot's existing build traversal, so the observer update is O(1) and does not rescan all
Projects. Rendering and the small output buffer allocation happen only on a Prometheus scrape,
never during configuration matching or request execution.

The Fiber registry continues to own request metric shards and their collection lifecycle. The
configuration block is appended after Fiber's text snapshot; no Fiber library modification or
cross-EventLoop `CounterRef`/`GaugeRef` mutation is required.

## Remaining scope

This increment covers Project List/route outcomes, route readiness, and route snapshot size/age.
Nacos client/naming connection state, service/endpoint aggregates, DNS/pool/proxy/WebSocket
outcomes, TLS rotation/reclaim, and async logging/CAT drops remain separate O-02 increments.
Typed, authenticated, per-instance activation evidence remains O-01 and must continue to be
reported as unknown until implemented.
