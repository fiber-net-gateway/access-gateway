# Instance activation evidence

## Purpose and trust boundary

Draft persistence, rnacos publication/readback, and activation on an individual `access-server`
remain three independent facts. A Release is `active` only when fresh, typed evidence from every
required instance exactly matches every required Release resource. The collector never infers
activation from rnacos readback, process liveness, metrics, or successful gateway traffic.

The native process exposes `GET /v1/activation-evidence` on its status listener. The endpoint is
disabled by default and requires all three settings:

```text
ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED=true
ACCESS_SERVER_INSTANCE_ID=access-prod-0
ACCESS_SERVER_ACTIVATION_TOKEN=<unique 32-512 byte printable ASCII bearer token>
```

`/metrics` keeps its existing behavior; the evidence path alone requires
`Authorization: Bearer <token>`. The token is compared in constant time, is never returned or
logged, and must be delivered only to the dedicated collector process. The control-plane API does
not need the token. Use HTTPS or a private authenticated service network because a bearer token
does not protect an unencrypted transport.

Container builds can pass `ACCESS_GATEWAY_BUILD_REVISION` as a 40- or 64-character source
revision. Source-tree builds derive it from Git and append `-dirty` when applicable; builds without
source metadata report `unknown` instead of inventing provenance.

## Native evidence contract

Contract version 1 reports:

- stable instance identity, build version/revision, and process start time;
- one immutable evidence revision and the process-wide route snapshot generation/fingerprint;
- Project List observed and active MD5 values, candidate state, readiness, and redacted failure;
- per-Project subscription state, observed MD5/version, active MD5/version, snapshot generation,
  and whether the Project is loaded in the published snapshot;
- gray and TLS observed/active digests, generations/versions, and bounded counts;
- bounded NamingService lifecycle and resource aggregates.

High-cardinality Project evidence is paginated. `limit` is 1-256. `nextCursor` binds the offset to
the evidence revision; if the revision changes during traversal, the server returns `409` and the
collector retries the complete traversal once. Revisions and generations are decimal strings so
JavaScript clients never lose 64-bit precision. Responses use `Cache-Control: no-store`, expose no
configuration payloads, endpoint addresses, headers, certificate material, or free-form error
messages, and retain only stable stage/code/field/offset failures.

Configuration watchers are the only writers on the Nacos owner EventLoop. They publish complete
immutable evidence snapshots through an atomic pointer. Status workers only pin a snapshot, so
the request hot path and watcher path do not acquire a shared mutex and readers cannot observe a
partially assembled model.

## Collector and persistence

Run the collector separately:

```bash
ACTIVATION_COLLECTOR_ENABLED=true \
ACTIVATION_TARGETS_JSON='[{"environmentCode":"prod","instanceKey":"access-prod-0","endpoint":"https://access-prod-0.internal:16689/v1/activation-evidence","token":"..."}]' \
npm run activation-collector --workspace server
```

Targets are bounded to 64 entries. Endpoint URLs, instance identities, response bytes, pages,
Projects, whole-snapshot request time, and concurrency are validated and bounded. Static target reconciliation
stores the instance identity and endpoint but never the bearer token. Database leases make polling
safe across collector replicas. Each observation has a TTL; old observations are removed in small
batches after seven days unless a current activation row still references them.

All collector replicas for one database must receive the same static target set. The evidence TTL
must exceed the poll interval, and the lease must be at least twice the whole-snapshot request
timeout; invalid or unbounded combinations fail during process configuration rather than surfacing
as lease churn at runtime. Observation expiry is calculated by MySQL so control-plane host clock
skew cannot extend or prematurely expire evidence.

When publication succeeds, the publication worker records the enabled instances required by that
Release. Later target additions are attached to currently published Releases during reconciliation.
Disabling a target removes it from the required aggregate without deleting historical evidence.

## Status calculation

The per-instance result is:

| Status     | Meaning                                                                                                                                     |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `active`   | Every required resource has the exact identity and active rnacos MD5/version; a removed Project is absent from the complete route snapshot. |
| `pending`  | Evidence is fresh, but at least one exact active value has not appeared and the target candidate has not explicitly failed.                 |
| `degraded` | Collection/auth/protocol failed, a target candidate was rejected, or a published Release lacks verifiable target metadata.                  |
| `unknown`  | No required target exists, no evidence exists, or the last evaluation has expired.                                                          |

The aggregate is `degraded` if any required instance is degraded, `unknown` if any required
instance lacks fresh evidence, `active` only if every required instance is active, and otherwise
`pending`. This ordering prevents partial success from being displayed as full activation.

The API returns the aggregate on Project and TLS Release views and exposes bounded per-instance
details at `GET /api/releases/:releaseId/activation?limit=50&cursor=<instance-id>`. Authorization
and environment scoping are identical to Release reads. Responses never expose collector endpoints
or tokens. The React Release page displays all four states and loads instance details on demand.

## Operational checks

- Give every instance a stable, environment-unique ID and a unique token.
- Keep the evidence TTL greater than the normal poll interval and smaller than the maximum stale
  interval operators are willing to accept.
- Alert on `degraded`, prolonged `pending`, or `unknown` for a published Release.
- Rotate a token by updating the instance and collector configuration together, then rolling the
  instance; tokens are process configuration and are not hot-reloaded.
- A collector outage naturally ages evidence to `unknown`; it never leaves an unbounded stale
  `active` result.

Production script-corpus differential verification and final cutover gates remain unfinished.
Instance evidence proves what the C++ runtime activated; it does not by itself certify complete
production compatibility.
