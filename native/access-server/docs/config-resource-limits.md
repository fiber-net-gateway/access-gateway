# Access configuration resource limits

`access-server` applies fixed, versioned limits before parsing, while decoding the wire model, and
while compiling immutable snapshots. These limits are product safety invariants rather than deployment
tuning knobs: every instance, the offline Native Validator, and the Console must agree on them.

The source of truth is `src/config/AccessConfigLimits.h`. Limit schema version 1 uses UTF-8 byte counts
for strings and payloads.

## Project List

| Resource | Limit |
| --- | ---: |
| Raw payload | 256 KiB |
| Project entries | 1,024 |
| Project name | 255 bytes |

## Project route

| Resource | Limit |
| --- | ---: |
| Raw/compiled payload | 4 MiB |
| Host patterns | 1,024 |
| Routes | 5,000 |
| Host pattern | 255 bytes |
| Path | 2,048 bytes |
| Method | 64 bytes |
| Service | 1,024 bytes |
| Cluster | 255 bytes |
| Condition | 256 KiB |
| Script | 1 MiB |
| Template or rewrite | 1 MiB |
| Entries in each header/context map | 256 |
| Header/context name | 256 bytes |
| Header/context value | 64 KiB |
| CIDRs per Route | 256 |
| CIDR | 64 bytes, plus the optional deny marker |
| Static addresses per Route | 256 |
| Static address | 2,048 bytes |
| Decoded static response body | 2 MiB |
| Static response storage per Project | 8 MiB |
| Path variables per pattern | 64 |
| Expressions per template | 256 |
| Compiled programs per Project | 20,000 |
| Estimated immutable snapshot memory | 64 MiB |

The snapshot estimate covers the repository-owned compiled route model, matcher nodes, strings,
templates, script handles, CIDR/address plans, and static response buffers. It is deliberately
conservative but is not a process RSS or allocator measurement. Static response accounting uses the
decoded identity representation and any precompressed representation actually retained by the
snapshot.

## Production gray rules

| Resource | Limit |
| --- | ---: |
| Raw payload | 256 KiB |
| Rule entries | 16 |
| Entry name | 64 bytes |
| CIDRs per entry | 256 |
| CIDR | 64 bytes |

## Failure and publication semantics

Oversized raw payloads are rejected before JSON tokenization. Container and string limits are checked
during decode, and expression/program/static-body/snapshot budgets are checked before publishing the
candidate. An over-limit candidate returns `LimitExceeded`; the Native Validator exposes the stable
code `limit_exceeded` and never echoes configuration content in its message.

Project route, Project List, and gray-rule failures retain the previous immutable state. An invalid
Project List does not reconcile subscriptions or unload projects. Readiness records the rejected
candidate and recovers only after a later valid list is processed. A successful rnacos write still does
not prove instance activation.

The offline validator exposes the exact schema without starting external clients:

```bash
./native/build/apps/access-gateway-validator --describe-config-limits
```

The Console probes this command at startup, validates the strict schema, publishes it through
`/api/system/status`, and uses it for draft compilation, Project List preparation, and editor feedback.
Release creation fails closed when a configured Native Validator or its limits probe is unavailable.
The server keeps a bounded schema-version-1 fallback only so draft transport remains constrained while
the validator is unavailable; the fallback is not publication evidence or authorization to publish.
