# Fibron Performance Benchmarks

This document summarizes the reproducible C++23 native microbenchmark suite for `access-server`. For
the full Chinese baseline report (including SHA-256 source/result manifests, resource tables, and
historical revision notes), see
[benchmark-baselines.md](../native/access-server/docs/benchmark-baselines.md).

> [!NOTE]
> Microbenchmarks measure hot-path CPU, allocation, and multi-worker scaling characteristics in
> isolation. They are designed to catch regressions and validate architecture changes—they do not
> replace end-to-end production capacity profiling.

## Running the Benchmarks Locally

```bash
# Full reproducible baseline (Release + LTO, pinned to 4 CPU cores)
ACCESS_SERVER_BENCHMARK_CPUSET=0-3 \
ACCESS_SERVER_BENCHMARK_MAX_WORKERS=4 \
NATIVE_BUILD_JOBS=4 \
npm run benchmark:native

# Quick verification run (fewer iterations, runs all 10 targets + correctness checks)
ACCESS_SERVER_BENCHMARK_QUICK=1 \
ACCESS_SERVER_BENCHMARK_MAX_WORKERS=2 \
npm run benchmark:native
```

All 10 benchmark targets run completely offline (no live rnacos, CAT, external DNS, or public
upstreams) and verify output checksums/invariants so a broken fast path fails non-zero.

## Reference Hardware & Build Environment

- **CPU:** Intel Core i7-13700H (pinned to CPUs `0–3`, max 4 workers via `taskset`)
- **OS / Kernel:** Linux 6.6.114 (x86_64)
- **Compiler:** Clang 22.1.6 (`Release + LTO`)

## Highlight Results

### 1. Eliminating Lock Contention: Worker-Sharded vs Shared-Mutex SWRR (4 Workers)

Same-binary A/B comparison across 1 to 128 upstream endpoints (4 workers, 100,000 ops × 21 samples).
Moving SWRR state and directory ownership into per-EventLoop shards while keeping healthy-path
circuit-breaker checks atomic-read-only yielded an **8x–10x multi-worker speedup** and raised
4-worker scaling efficiency from `<10%` to **75–87%**:

| Mode            | Endpoints | Shared Mutex p50 / p99 | Worker-Sharded p50 / p99 | Sharded Throughput |    Speedup |
| --------------- | --------: | ---------------------: | -----------------------: | -----------------: | ---------: |
| Select-only     |         1 |       175.2 / 198.7 ns |           19.0 / 21.2 ns |  **52,702,005 /s** |  **9.23x** |
| Select-only     |         8 |       195.8 / 239.0 ns |           22.2 / 24.2 ns |  **44,958,409 /s** |  **8.80x** |
| Select-only     |        32 |       366.0 / 400.8 ns |           42.3 / 54.9 ns |  **23,669,110 /s** |  **8.66x** |
| Select-only     |       128 |       923.4 / 951.2 ns |         112.7 / 128.9 ns |   **8,874,173 /s** |  **8.19x** |
| Select + report |         1 |       276.3 / 300.6 ns |           27.3 / 32.3 ns |  **36,622,980 /s** | **10.12x** |
| Select + report |         8 |       299.8 / 336.5 ns |           30.7 / 40.8 ns |  **32,616,648 /s** |  **9.78x** |
| Select + report |        32 |       440.7 / 459.2 ns |           55.0 / 96.2 ns |  **18,196,912 /s** |  **8.02x** |
| Select + report |       128 |       932.6 / 999.5 ns |         115.8 / 122.6 ns |   **8,636,208 /s** |  **8.05x** |

### 2. Routing, Gray Match, and SNI TLS Selection

| Benchmark Case                            | Workers |   p50 ns |   p99 ns | Throughput       |
| ----------------------------------------- | ------: | -------: | -------: | ---------------- |
| Route snapshot pin (worker-local)         |       4 |     5.75 |    19.57 | **173.8M ops/s** |
| Route lookup (352 projects, 1,056 routes) |       4 |    45.92 |    84.03 | **21.8M ops/s**  |
| Host matcher (exact, 1 → 1,024 fan-out)   |       1 | 54.9→136 | 59.3→167 | **7.35M+ ops/s** |
| Gray match (ratio hit)                    |       4 |     6.32 |    12.66 | **158.2M ops/s** |
| Gray match (worst-case 256th CIDR)        |       4 |    82.91 |   149.33 | **12.1M ops/s**  |
| TLS SNI select (exact SAN)                |       1 |    29.36 |    34.41 | **34.1M ops/s**  |
| TLS SNI select (wildcard SAN)             |       1 |    28.50 |    29.22 | **35.1M ops/s**  |
| TLS snapshot commit + reclaim             |       1 |    4,620 |   41,478 | **216.5K ops/s** |

### 3. Allocations, Config Publication, and Loopback Proxy

| Benchmark Case                         | p50 Latency | Allocs / op | Notes                                |
| -------------------------------------- | ----------: | ----------: | ------------------------------------ |
| Static template rendering              |    `1.7 ns` |         `0` | `0 B/op`                             |
| Dynamic template rendering             |   `27.5 ns` |         `1` | `268 B/op`                           |
| Batch compile + publish (352 projects) |   `1.02 ms` |           — | **79.8x** faster than per-project    |
| Real HTTP loopback proxy (end-to-end)  |  `36.04 us` |        `57` | `99.995%` connection pool hit ratio  |
| Real WebSocket raw tunnel (end-to-end) | `112.86 us` |           — | Full HTTP/1.1 upgrade + frame tunnel |
| Async access logger (10% sampling)     |   `29.3 ns` |       `0.1` | `0` queue drops                      |

### 4. Memory Footprint

Across all core routing, Host matcher, gray match, TLS identity, and loopback proxy targets, max
process RSS stays between **3.8 MiB and 6.5 MiB** (`12.1 MiB` for the full observability target with
async logger + CAT aggregation buffers).
