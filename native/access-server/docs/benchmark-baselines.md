# Access Server 性能基准与复现契约

## 1. 目的和边界

本文记录 T-02 的可复现基线，覆盖 access-server 请求热路径、配置发布、TLS identity、
可观测性，以及当前 pinned Fiber DNS/connector 的真实 loopback 集成。所有 benchmark target
默认关闭，不链接进生产应用；fixture 不访问 rnacos、CAT collector、公网 DNS 或生产上游。

这些结果用于发现回归、比较实现方案和决定是否继续优化，不能单独证明生产容量、Java 兼容或
切流条件。完整生产 profile、请求级 Java/C++ differential 和阶段 8 仍由 D-01 门禁管理。

## 2. 统一运行入口

完整基线使用：

```bash
ACCESS_SERVER_BENCHMARK_CPUSET=0-3 \
ACCESS_SERVER_BENCHMARK_MAX_WORKERS=4 \
NATIVE_BUILD_JOBS=4 \
npm run benchmark:native
```

快速验证使用较少 operation，但仍执行全部 target 和正确性检查：

```bash
ACCESS_SERVER_BENCHMARK_QUICK=1 \
ACCESS_SERVER_BENCHMARK_MAX_WORKERS=2 \
npm run benchmark:native
```

[`run_benchmarks.sh`](../scripts/run_benchmarks.sh) 固定配置 Release、LTO、关闭测试并显式打开
`ACCESS_SERVER_BUILD_BENCHMARKS`，然后构建和运行十个 target。可用环境变量如下：

| 变量 | 默认值 | 作用 |
| --- | --- | --- |
| `ACCESS_SERVER_BENCHMARK_BUILD_DIR` | `native/build-benchmarks` | 独立 CMake build tree |
| `ACCESS_SERVER_BENCHMARK_RESULT_DIR` | build tree 下的 `results` | 原始结果目录 |
| `ACCESS_SERVER_BENCHMARK_MAX_WORKERS` | `4` | 多 worker case 上限 |
| `ACCESS_SERVER_BENCHMARK_CPUSET` | 不限制 | 用 `taskset` 约束每个 target；非法或无权限时 fail closed |
| `ACCESS_SERVER_BENCHMARK_QUICK` | `0` | `1` 只缩小 operation/session 数，不跳过 target |
| `NATIVE_BUILD_JOBS` | `2` | 构建并行度 |

每个 target 产生 CSV、stderr 和 `.resources`。资源文件由 GNU time 记录 user/system CPU、
平均 CPU 百分比、最大 RSS 和 wall time。metadata 记录 revision、dirty 状态、benchmark source
digest、实际 native source digest、Fiber revision、编译器、内核、CPU、绑核范围和运行档位；
`manifest.sha256` 对所有结果再逐文件校验。
这些原始结果位于被忽略的 build tree，不提交仓库。

## 3. 覆盖矩阵

| Target | 覆盖路径 | 主要输出 |
| --- | --- | --- |
| `fiber_access_service_selection_benchmark` | canonical/worker-sharded directory pin、cluster lookup、SWRR select 与 report | mode/completion/endpoint/worker p50/p95/p99、吞吐、扩展效率 |
| `fiber_access_template_header_benchmark` | 静态/动态 template、response/proxy headers | 延迟、吞吐、分配次数和字节 |
| `fiber_access_route_publication_benchmark` | sequential/batch commit、完整 compile/build/publish | 10/100/352/500 项目更新时间和发布次数 |
| `fiber_access_host_matcher_benchmark` | exact Host 高 fan-out hit/miss | 1..1024 fan-out 延迟和吞吐 |
| `fiber_access_route_lookup_benchmark` | 352 项目、1,056 routes、global/worker pin、Host/Path | 多 worker 延迟和吞吐 |
| `fiber_access_gray_match_benchmark` | ratio、首/末 CIDR、entry miss | 256 CIDR 最坏扫描和多 worker 扩展 |
| `fiber_access_tls_identity_benchmark` | exact/wildcard/default identity、prepare、rotate/reclaim | 延迟、吞吐、rotation/reclaim/最长保留 |
| `fiber_access_proxy_loopback_benchmark` | 真实 HTTP gateway/upstream、pool reuse、raw WebSocket tunnel | 端到端延迟、分配、响应字节、pool hit ratio |
| `fiber_access_observability_benchmark` | access policy、async logger、CAT disabled/0%/100% | 延迟、吞吐、分配、queue/drop 计数 |
| `fiber_access_network_integration_benchmark` | 双 nameserver failover/timeout、IPv4、IPv6 失败后 IPv4 | 真实 UDP/TCP loopback 延迟和吞吐 |

每个计时 case 同时做 fixture/result/checksum 校验；DNS、connector、proxy、WebSocket、TLS
rotation 和 CAT lifecycle 任一结果异常都会使 target 非零退出。WebSocket 当前只记录会话延迟和
响应字节，CSV 用 `NA` 明确表示没有单独测量分配和 pool hit，避免把未测量误报为零。

## 4. 2026-08-18 T-02 初始基线（P-01 改造前）

### 4.1 环境和证据标识

| 字段 | 值 |
| --- | --- |
| 本地时间 | 2026-08-18 |
| UTC timestamp | `20260817T170416Z` |
| parent revision | `ea4bb2978d1988dee7b0978db28b919e06b5ec48` |
| source 状态 | `dirty=true`；本工作项代码在提交前测量 |
| benchmark source SHA-256 | `7bd7efa710aba3290508cefb2c5cc24487c48d49bc43fe96cf7275200c9c1957` |
| result manifest SHA-256 | `933f153626971a544e355bebf687849dbae4c95dd6f6e2f8f68cedba2ebe8426` |
| 构建 | Release + LTO |
| 编译器 | Ubuntu Clang 22.1.6，`/usr/bin/clang++-22` |
| 内核 | Linux 6.6.114.1-microsoft-standard-WSL2 x86_64 |
| CPU | 13th Gen Intel Core i7-13700H，20 logical CPUs |
| 测量 affinity | CPUs `0-3`，最多 4 workers |

source digest 覆盖 `CMakeLists.txt`、runner 和全部 benchmark `.cpp`/`.h`，用于精确区分
提交前 dirty tree。result manifest digest 对应本节采用的完整 CSV/stderr/resource 集合。

### 4.2 进程资源

CPU 百分比除以 100 即运行期间平均使用的 core 数；它包含 target 自身创建的 EventLoop、日志和
loopback server 线程。

| Target | user s | system s | CPU | 平均 cores | wall s | max RSS KiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| service selection | 9.29 | 5.63 | 193% | 1.93 | 7.70 | 4,000 |
| template/header | 0.59 | 0.00 | 98% | 0.98 | 0.60 | 4,000 |
| route publication | 2.58 | 0.00 | 98% | 0.98 | 2.62 | 5,964 |
| Host matcher | 0.86 | 0.00 | 99% | 0.99 | 0.87 | 4,320 |
| route lookup | 9.21 | 0.03 | 242% | 2.42 | 3.81 | 6,452 |
| gray match | 4.19 | 0.03 | 216% | 2.16 | 1.95 | 3,840 |
| TLS identity | 0.17 | 0.00 | 100% | 1.00 | 0.17 | 5,760 |
| proxy/WebSocket | 0.35 | 0.55 | 112% | 1.12 | 0.80 | 6,080 |
| observability | 1.85 | 0.01 | 100% | 1.00 | 1.85 | 12,072 |
| DNS/connector | 0.00 | 0.04 | 4% | 0.04 | 1.27 | 4,684 |

### 4.3 Selection、route 和 gray

本节 service selection 数据是 P-01 第二阶段前的 canonical-only 历史基线；当前 target 已扩展为
canonical/worker-sharded 与 select-only/select-report 矩阵，改造后对照见第 5 节。operation 是固定
总 selection 数，不随 worker 数倍增。扩展效率为
`N-worker throughput / (1-worker throughput * N)`；它是共享路径竞争的代理指标，不是
mutex wait time 的直接采样。

| endpoints | workers | p50 ns | p95 ns | p99 ns | ops/s | 扩展效率 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 56.28 | 57.73 | 58.71 | 17,767,294 | 1.0000 |
| 1 | 4 | 144.20 | 158.68 | 180.44 | 6,934,807 | 0.0976 |
| 8 | 1 | 59.30 | 66.37 | 73.13 | 16,864,214 | 1.0000 |
| 8 | 4 | 166.76 | 175.28 | 176.05 | 5,996,604 | 0.0889 |
| 32 | 1 | 92.92 | 99.40 | 111.29 | 10,761,640 | 1.0000 |
| 32 | 4 | 332.60 | 365.35 | 372.08 | 3,006,603 | 0.0698 |
| 128 | 1 | 220.70 | 262.18 | 300.73 | 4,531,126 | 1.0000 |
| 128 | 4 | 596.39 | 605.96 | 610.09 | 1,676,750 | 0.0925 |

代表性 route/gray 结果：

| Case | workers | p50 ns | p95 ns | p99 ns | ops/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| route `global_pin_only` | 4 | 140.90 | 147.71 | 149.23 | 7,097,055 |
| route `worker_pin_only` | 4 | 5.75 | 13.33 | 19.57 | 173,808,154 |
| route `worker_pin_lookup` | 4 | 45.92 | 83.83 | 84.03 | 21,774,780 |
| gray ratio hit | 4 | 6.32 | 12.52 | 12.66 | 158,227,097 |
| gray first CIDR hit | 4 | 6.76 | 13.05 | 13.26 | 147,927,681 |
| gray last of 256 CIDRs | 4 | 82.91 | 149.21 | 149.33 | 12,061,732 |

Host matcher 从 fan-out 1 的 p50/p99 `54.93/59.30 ns` 增长到 fan-out 1024 的
`136.14/167.29 ns`，仍保持约 7.35M lookup/s。

### 4.4 分配、配置和 TLS

| Case | p50 ns | p95 ns | p99 ns | allocations/op | bytes/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| static template | 1.7 | 1.9 | 3.0 | 0 | 0 |
| dynamic template | 27.5 | 30.8 | 32.3 | 1 | 268 |
| static response headers | 416.0 | 453.5 | 453.5 | 1 | 640 |
| dynamic response headers | 564.0 | 595.7 | 597.5 | 9 | 1,272 |
| static proxy headers | 319.4 | 357.9 | 385.5 | 1 | 640 |
| dynamic proxy headers | 536.8 | 555.9 | 589.7 | 9 | 1,272 |

352 项目完整 `config create -> compile -> prepare -> ready -> commit_batch` 的 p50/p95/p99 为
`1023.0/1051.7/1057.3 us`，约 344,094 projects/s；仅 batch commit p50 为 `380.2 us`。
相同 prepared candidates 逐项目 commit 的 p50 为 `30346.4 us`，batch commit 快 79.81 倍。

| TLS case | p50 ns | p95 ns | p99 ns | ops/s |
| --- | ---: | ---: | ---: | ---: |
| exact select | 29.36 | 31.49 | 34.41 | 34,056,802 |
| wildcard select | 28.50 | 28.86 | 29.22 | 35,092,214 |
| default select | 25.03 | 25.69 | 25.77 | 39,950,988 |
| snapshot prepare | 33,115 | 79,608 | 85,496 | 30,198 |
| commit + reclaim | 4,620 | 9,155 | 41,478 | 216,450 |

21 次 rotation 全部被 observer 记录，23 次 reclaim 回收 22 个 snapshot。最长保留时间记录为
`0 ns`，含义是本 fixture 的 retired snapshot 在同一 EventLoop cached-clock tick 内回收；它不是
墙钟精度的“绝对零”，生产保留上界仍须结合 runtime 指标观察。

### 4.5 Proxy、可观测性和网络集成

| Case | p50 ns | p95 ns | p99 ns | ops/s | allocations/op | bytes/op | 其他 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| HTTP loopback proxy | 36,036.76 | 37,577.85 | 38,221.38 | 27,749 | 57.036 | 53,929.92 | pool hit 0.999955 |
| WebSocket raw tunnel | 112,863 | 160,264 | 176,977 | 8,860 | NA | NA | 186 response bytes/session |
| access log 10% | 23.87 | 27.22 | 27.61 | 41,898,540 | 0.100 | 25.70 | policy + URI rendering |
| access log all | 247.00 | 250.91 | 251.27 | 4,048,559 | 1 | 257 | policy + URI rendering |
| async logger 10% | 29.28 | 38.72 | 60.99 | 34,153,005 | 0.101 | 94.54 | `/dev/null` appender |
| async logger all | 610.97 | 682.84 | 779.54 | 1,636,731 | 1 | 936 | `/dev/null` appender |
| CAT sampled out | 268.05 | 282.71 | 312.04 | 3,730,661 | 0 | 0 | 22,000 trees aggregated |
| CAT sampled in | 610.22 | 645.71 | 658.00 | 1,638,753 | 2 | 286 | 22,000 messages submitted |

async logger 峰值排队 831、drop 0；CAT sampled-in 的 queue-full drop 为 0。

| Network case | p50 us | p95 us | p99 us | ops/s |
| --- | ---: | ---: | ---: | ---: |
| DNS SERVFAIL 后第二 nameserver 成功 | 13.797 | 15.107 | 72.239 | 72,480 |
| DNS 2ms timeout 后第二 nameserver 成功 | 2,162.611 | 2,309.021 | 2,323.823 | 462 |
| connector IPv4 | 29.682 | 55.673 | 204.026 | 33,690 |
| connector IPv6 失败后 IPv4 | 10,220.026 | 10,381.205 | 10,443.995 | 98 |

最后一项包含当前 Fiber Happy Eyeballs 最小 attempt delay；IPv6 loopback 不可用时 target 会明确
输出 `SKIP`，不能伪造为通过数据。

## 5. P-01 worker-sharded 对照基线

### 5.1 环境和证据标识

本轮使用与第 4 节相同机器、绑核、Release+LTO 和 full operation 口径；同一 binary 内依次运行
canonical 与 worker-sharded，减少跨构建比较误差。

| 字段 | 值 |
| --- | --- |
| 本地时间 | 2026-08-18 |
| UTC timestamp | `20260817T173556Z` |
| parent revision | `2afa6d4b57d924e0195cc6ae24c7c5202dd900d0` |
| source 状态 | `dirty=true`；P-01 代码和文档在提交前测量 |
| benchmark source SHA-256 | `9ff60d2df8e1d3c378d829f535ba8b608359e403999a3f0af54e6fcb2251eb67` |
| native source SHA-256 | `add48727c2e90b33defbe430bd14774484189002e08949ce504e36b162662189` |
| Fiber revision | `abc8c34ba13bd50554a55e10389c6b3da2dcc048` |
| result manifest SHA-256 | `bbe37b198df0d6ac9f6b7431256092592381e3cccac8578c57c0b7c69e1d49bb` |
| 构建/affinity | Release + LTO；CPUs `0-3`；最多 4 workers |
| 编译器/内核/CPU | Ubuntu Clang 22.1.6；Linux 6.6.114.1 WSL2；i7-13700H |

`native_source_sha256` 覆盖 native/access-server 的生产 `src/`、全部 benchmark、runner、组件及
native 顶层 CMake；因此即使结果来自提交前 dirty tree，也能精确区分实际被测实现。service
selection target 的资源总量为 user `41.40 s`、system `17.33 s`、平均 `198%` CPU、wall
`29.56 s`、最大 RSS `4,000 KiB`；运行时间增加是因为现在覆盖两种 mode 和两种 completion，不能
直接与第 4 节 canonical-only target wall time比较。

### 5.2 Select-only 对照

以下均为 4 worker、每 aggregate sample 固定总计 100,000 次操作、21 个样本。扩展效率仍定义为
`4-worker throughput / (1-worker throughput * 4)`；加速比是同一轮 `worker-sharded / canonical`
四 worker 吞吐。

| endpoints | canonical p50/p95/p99 ns | canonical ops/s | sharded p50/p95/p99 ns | sharded ops/s | sharded 扩展效率 | 加速 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 175.23 / 185.91 / 198.74 | 5,706,753 | 18.97 / 20.63 / 21.20 | 52,702,005 | 86.57% | 9.23x |
| 8 | 195.82 / 212.51 / 238.97 | 5,106,679 | 22.24 / 23.23 / 24.22 | 44,958,409 | 82.06% | 8.80x |
| 32 | 366.02 / 392.11 / 400.83 | 2,732,091 | 42.25 / 50.46 / 54.86 | 23,669,110 | 82.55% | 8.66x |
| 128 | 923.38 / 949.39 / 951.20 | 1,082,982 | 112.69 / 124.20 / 128.94 | 8,874,173 | 80.44% | 8.19x |

### 5.3 Select + report 对照

该模式每次成功选择后立即走生产 completion 路径。健康 success 不获取共享 circuit mutex；它仍
更新 worker-local SWRR 权重状态。failure/half-open 路径由正确性与 sanitizer 测试覆盖，不用正常
成功流量伪造故障比例。

| endpoints | canonical p50/p95/p99 ns | canonical ops/s | sharded p50/p95/p99 ns | sharded ops/s | sharded 扩展效率 | 加速 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 276.32 / 300.31 / 300.61 | 3,619,022 | 27.31 / 31.28 / 32.28 | 36,622,980 | 84.65% | 10.12x |
| 8 | 299.82 / 331.69 / 336.48 | 3,335,350 | 30.66 / 36.77 / 40.79 | 32,616,648 | 84.63% | 9.78x |
| 32 | 440.70 / 453.14 / 459.20 | 2,269,143 | 54.95 / 76.43 / 96.21 | 18,196,912 | 75.77% | 8.02x |
| 128 | 932.57 / 991.93 / 999.47 | 1,072,304 | 115.79 / 122.32 / 122.55 | 8,636,208 | 84.52% | 8.05x |

worker-sharded 的单 worker 吞吐相对同轮 canonical 为 `-7.6%..+1.3%`。四 worker 的明显改善来自
把 directory ownership control block、SWRR mutex、current/effective weight 和 completion state
限制到所属 EventLoop；endpoint scan 仍为每 worker O(N)，共享 circuit 在健康路径只做 bounded
atomic read。该微基准证明实现消除了原共享热点并排除了明显单线程回退，但不证明生产 QPS、故障
比例或 endpoint 分布下的收益。

## 6. 解释和后续比较规则

- 在同一机器比较前后版本时，保持 build type、LTO、CPU affinity、worker 数和 runner 档位一致，
  并同时保存 metadata 与 manifest；不能把 quick 与 full 数值混用。
- 第 4 节 service selection 已稳定复现负扩展并触发 P-01；第 5 节同 binary 对照证明项目内
  worker sharding 消除了该共享路径的主要扩展瓶颈，但仍不能把每个子组件的 CPU 占比或真实生产
  收益精确归因。最终容量决策仍需真实 service/endpoint/worker/故障分布的 profile。
- loopback proxy 数值包含 client socket、gateway、upstream、Fiber buffers 和线程调度，适合做同机
  回归，不等同于网络环境中的单请求 SLA。
- GNU time 的 CPU/RSS 是 target 级总量，单个 CSV case 的 CPU attribution 需要 `perf` 或生产
  profiler；本 runner 不以无权限时的空 `perf` 数据冒充锁等待证据。
- 只有跨应用复用需要新增 Fiber SWRR/RCU 抽象，或改变 DNS/connector 实现时，才先在 Fiber 上游
  增加组件 benchmark，再更新 gitlink 并复跑这里的集成基线；P-01 当前实现不等待上游化。
