# Access Server 全面优化分析与代码归属

## 1. 文档目的

本文对 `native/access-server` 的代码实现、类职责、生命周期、并发模型、请求热路径、
配置热更新、安全边界、可观测性、构建和测试体系进行系统审阅，并区分每项改造的代码
所有权：

- **本项目**：在 `native/access-server/`，或同一 Access Gateway 仓库的 `server/`、
  `web/`、`docs/` 中完成；
- **Fiber**：属于可复用事件、网络、DNS、HTTP、连接池、TLS、负载均衡或并发基础设施，
  应先贡献到 `fiber-gateway-cpp` 上游；
- **双方**：Fiber 提供通用机制，Access Gateway 负责产品策略、Java 兼容、配置接入、
  指标和回归验证。

“双方”不表示直接修改 `third_party/fiber-gateway-cpp/`。Fiber 改动必须先形成独立上游
Issue/PR，合入后再审查 revision range、运行完整回归并更新 gitlink。一般情况下不得在
本仓库直接修改 submodule；临时兼容补丁仍须遵守 `native/patches/` 的约束。

## 2. 审阅范围和结论可信度

本次审阅基于：

- Access Gateway revision：`0f7b557`；
- Fiber pinned revision：`0fda7764bf94944aca4b674ab5ab311184703118`；
- 审阅日期：2026-08-16；
- `native/access-server/src/` 约 1.37 万行代码；
- 当前 Release/ThinLTO 构建、CMake target、兼容文档和测试注册；
- `ctest --test-dir native/build --output-on-failure -L access-server`：共发现 170 个测试，
  0 失败，其中
  `ProductionScriptCorpusTest.CompilesExternalSnapshotWhenProvided` 因未设置私有 corpus 而
  skip。

本文将结论分成两类：

1. **代码可以直接确认的问题**：同步等待、readiness 误判、失败不可见、敏感 URI 日志、
   配置缺少上限等；
2. **需要测量确认的性能候选**：共享锁、atomic shared pointer、字符串分配、Host matcher
   子节点搜索、coroutine frame 等。

没有生产流量 profile、完整生产脚本 corpus 和阶段 8 全量差分，因此本文不据此宣称已
达到生产兼容或切流条件。

## 3. 总体评价

`access-server` 已经具备较好的数据面基础：

- 路由、项目和 TLS 使用完整编译后再发布的不可变快照；
- 非法配置保留旧快照，同版本候选忽略，请求 pin 住其执行版本；
- 路由条件、模板、CIDR 和静态 gzip 尽量前移到配置编译期；
- proxy body、response body 和 WebSocket 使用流式转发；
- 连接池 lease、service generation 和请求快照生命周期有明确绑定；
- 指标按 worker 预绑定固定 schema，没有把任意 project/route/cluster 作为 label；
- 异步日志采用有界队列和明确的过载丢弃策略；
- C++ 错误通过 `expected`/result 类型传播，没有在请求路径引入异常。

当前主要问题不是基础架构方向错误，而是以下几类系统性缺口：

1. 生命周期中仍存在 EventLoop 同步阻塞；
2. “收到配置”“编译发布”“实例可服务”之间的状态没有完整建模；
3. Nacos owner loop 承担了过多 CPU 和分配工作；
4. service selection、gray sampling 和 snapshot pin 存在跨 worker 共享状态；
5. 安全边界依赖部署约定，未在代码中显式表达；
6. 大类内部边界和 CMake target 边界不足，限制了测试和后续演进。

## 4. 优先级与归属总表

| ID | 优先级 | 优化项 | 归属 | 是否需要 Fiber 前置 |
| --- | --- | --- | --- | --- |
| L-01 | P0 | DNS shutdown 全异步化 | 本项目 | 否 |
| L-02 | P0 | 项目订阅失败重试和精确 readiness | 本项目 | 否 |
| L-03 | P0 | 路由配置字节、数量和编译内存上限 | 本项目 | 否 |
| L-04 | P1 | 配置编译移出 Nacos owner loop | 本项目 | 否 |
| L-05 | P1 | prepared/ready/published typestate | 本项目 | 否 |
| L-06 | P1 | 系统 DNS 配置、多 nameserver 和 failover | 双方 | 是 |
| S-01 | P0 | access log query 脱敏 | 本项目 | 否 |
| S-02 | P0/P1 | trusted proxy 和真实客户端地址模型 | 本项目 | 否 |
| S-03 | P1 | 上游 TLS peer/CA/SNI 验证配置 | 本项目 | 否，Fiber 已具备能力 |
| S-04 | P1 | 上游 mTLS 客户端身份 | 双方 | 是 |
| P-01 | P1 | 消除 service selection 双层共享锁 | 双方 | 通用 SWRR 上游化时需要 |
| P-02 | P1/P2 | per-worker route snapshot/RCU pin | 双方 | 采用通用原语时需要 |
| P-03 | P1 | per-worker gray snapshot 和 PRNG | 本项目 | 否 |
| P-04 | P1 | TLS hazard reaper 只在有退休对象时调度 | 本项目 | 否 |
| P-05 | P1 | 模板、request target、header 分配优化 | 本项目 | 否 |
| P-06 | P1 | 初始项目批量发布和全局 matcher 重建优化 | 本项目 | 否 |
| P-07 | P2 | Host matcher 高 fan-out 搜索优化 | 本项目 | 否 |
| P-08 | P1 | Happy Eyeballs/交错多地址连接 | 双方 | 是 |
| O-01 | P0/P1 | 实例级配置激活证据 | 本项目 | 否 |
| O-02 | P1 | 配置、DNS、pool、proxy、TLS、日志指标 | 本项目 | 否，Fiber 已暴露部分统计 |
| C-01 | P1/P2 | 大类职责拆分 | 本项目 | 否 |
| C-02 | P2 | 拆分 `access_server_core` 构建边界 | 本项目 | 否 |
| T-01 | P0/P1 | 生命周期、并发、sanitizer、fuzz 测试 | 本项目 | Fiber 改动另跑上游测试 |
| T-02 | P1 | 请求、selector、TLS、配置编译 benchmark | 双方 | Fiber 通用组件需上游 benchmark |
| D-01 | P1 | 生产 corpus 和阶段 8 门禁状态统一 | 本项目 | 否 |

P0 表示应在性能重构前处理的正确性、安全或生命周期问题；P1 表示高收益改造；P2 表示
应由 profile、规模数据或维护成本触发的结构优化。

### 4.1 需要 Fiber 实际改动的清单

实施对应能力时，以下事项确定需要 Fiber 上游前置：

- **L-06**：系统 DNS 配置、多 nameserver 和 failover；
- **S-04**：TLS client context 加载客户端证书和私钥；
- **P-08**：可取消、可复用的 Happy Eyeballs 多地址 connector。

以下事项只有在 benchmark 或多应用复用需求成立后，才建议引入 Fiber 改动：

- **P-01**：将通用 SWRR、selection token 和 circuit state 上游化或提供 sharded
  balancer；
- **P-02**：提供通用 loop-local immutable snapshot/epoch/RCU 原语；
- **T-02**：为上述通用组件建立 Fiber benchmark。

其余事项均可由本项目独立完成。即使归属为“双方”，端到端交付也必然包括本项目的配置、
兼容、指标和测试工作，因此没有仅更新 Fiber gitlink 即可完成的产品优化项。

## 5. 生命周期、配置和并发正确性

### 5.1 L-01：DNS shutdown 全异步化

**归属：本项目。Fiber 无前置改动。**

**实施状态：已解决（2026-08-16）。** DNS 初始化和关闭已纳入同一协程状态机，resolver
释放任务在各自 owner loop 执行并通过 `async::WaitGroup` 异步汇合，随后在 cache owner loop
关闭共享 cache。正常关闭、初始化回滚和重复关闭不再阻塞 control EventLoop；worker group
仍须遵守“DNS 关闭完成后才能停止 worker”的生命周期契约。回归测试会在 worker 暂停处理
任务时验证 control loop 仍能继续调度。

改造前，`AccessDnsService::shutdown()` 向 HTTP worker 投递 resolver 释放任务后，两次调用
`std::future::wait()`。它由 accept EventLoop 上的 `AccessServer::shutdown_and_wait()` 调用，
因此会阻塞 EventLoop；当目标 worker 已停止处理任务时还可能永久等待。

代码位置：

- [`AccessDnsService.cpp`](../src/runtime/AccessDnsService.cpp#L88)；
- [`AccessServer.cpp`](../src/runtime/AccessServer.cpp#L90)。

实施方案：

1. 将 `AccessDnsService::shutdown()` 改为 `async::Task<void>`；
2. 使用 `async::WaitGroup`、`Watch` 或等价 coroutine join；
3. 在各 resolver 的 owner loop 调用 `release()`，最后在 cache owner loop 关闭 cache；
4. 初始化中途失败也进入同一异步清理状态机；
5. 测试正常 shutdown、部分初始化、重复 shutdown、worker 已请求停止和 startup 中断。

Fiber 的 `DnsResolverLocal::release()` 和 `DnsResolver::release()` 已提供 owner-loop 释放
接口，缺少的是 access-server 对多个 worker 的异步编排，因此该问题不应通过修改
submodule 解决。

### 5.2 L-02：项目订阅失败和 readiness 状态建模

**归属：本项目。**

当前 `AccessConfigWatcher::apply_project_list()` 在完成项目列表 reconcile 后立即发布
ready。单项目 `subscribe()` 失败时，`add_project()` 只删除 entry 并增加
`failed_updates_`，既不 retry，也不记录包含 Data ID 和错误原因的 `last_failure_`。

代码位置：

- [`AccessConfigWatcher.cpp`](../src/runtime/AccessConfigWatcher.cpp#L150)；
- [`AccessConfigWatcher.cpp`](../src/runtime/AccessConfigWatcher.cpp#L270)；
- [`AccessServerRuntime.cpp`](../src/runtime/AccessServerRuntime.cpp#L338)。

实例可能已经绑定 listener，但部分项目从未建立订阅。请求仍然 fail closed，但运维上
会产生“实例已就绪”的错误判断。

建议为每个项目持久维护以下状态：

- `desired`；
- `subscribing/subscribed/retrying/retiring`；
- `first_value_received`；
- `observed_md5` 和 route version；
- `decode_result/compile_result/service_ready_result`；
- `published_generation`；
- `last_error` 和 `next_retry_at`。

初始订阅失败应采用有上限的指数退避，并区分：

- liveness：进程和 EventLoop 正常；
- control-plane connected：Nacos config/naming 已连接；
- readiness：要求的初始资源已经达到明确条件；
- activation：本实例已经发布某个编译快照。

是否在项目全部 ready 前绑定 socket 可以由产品决定，但 readiness 不能再等同于
“看到了 project-list 首值”。

### 5.3 L-03：配置资源上限

**归属：本项目，且必须同步 native runtime、validator、server 和 web。**

项目 route codec 会直接解析完整 payload，但当前没有统一的 payload、项目、route、
host、header、CIDR、address、script、template 和预压缩 body 上限。TLS codec 已经有
较完整的限制，可以作为实现范式。

代码位置：

- [`AccessConfigCodec.cpp`](../src/config/AccessConfigCodec.cpp#L969)；
- [`ProjectRouteSnapshot.cpp`](../src/routing/ProjectRouteSnapshot.cpp#L785)；
- [`TlsCertificateConfig.h`](../src/config/TlsCertificateConfig.h#L13)；
- [`NativeValidatorProtocol.h`](../src/validation/NativeValidatorProtocol.h#L10)。

建议增加共享的 `AccessConfigLimits`，至少包含：

- project-list 总字节、项目数和项目名长度；
- 单项目 route snapshot 原始字节；
- host、route、header、CIDR、address 数量；
- path、method、condition、script、template、header name/value 长度；
- 单响应 identity/gzip body 和整个项目静态响应总字节；
- 最大 path variable、template expression 和 compiled program 数量；
- 编译后 snapshot 的近似内存预算。

超限必须返回稳定的 `LimitExceeded`、字段路径和脱敏消息，保留旧快照。Console validator
和运行时必须引用同一份 native 限制，避免控制面接受数据面必然拒绝的内容。

### 5.4 L-04：配置编译移出 Nacos owner loop

**归属：本项目。初始实现不需要 Fiber 改动。**

项目回调当前在 Nacos loop 上完成 JSON decode、关系校验、脚本和模板编译、CIDR/address
编译以及静态 gzip；TLS watcher 也会在 owner loop 上解析 PEM 和创建 TLS context。大配置
或复杂脚本会延迟同一 loop 上的其他配置和 NamingService 工作。

建议建立一个 access-server 专用、有界的 compiler worker 或 compiler loop：

```text
Nacos owner loop
  -> 检查原始字节上限并记录 generation
  -> compiler worker 纯 decode/validate/compile
  -> 回到 Nacos loop 检查 generation
  -> 获取 service lease 并等待 ready
  -> 原子发布
```

脚本 compiler、OpenSSL context 或其他组件如果具有 thread/loop 亲和性，应在 compiler
worker 内构造独立实例。任务队列必须有容量、取消和“新 generation 替换旧任务”的明确
策略。

若未来需要通用 CPU work executor，可单独贡献到 Fiber；本项不应因等待通用抽象而阻塞
本项目的专用实现。

### 5.5 L-05：配置发布 typestate

**归属：本项目。**

`ProjectRouteSnapshot::wait_ready()` 是异步 readiness，而 Nacos selector 的
`ready_for_publish()` 恒为 false。watcher 等待成功后直接调用 `commit()`，从而绕过
`RouteConfigStore::apply()` 的同步检查。这种 API 容易被新的调用方误用。

代码位置：

- [`ProjectRouteSnapshot.cpp`](../src/routing/ProjectRouteSnapshot.cpp#L754)；
- [`AccessServiceDiscovery.cpp`](../src/runtime/AccessServiceDiscovery.cpp#L351)；
- [`RouteConfigStore.cpp`](../src/runtime/RouteConfigStore.cpp#L84)。

建议把状态表达为不同类型或只暴露单向状态转换：

```text
ParsedProjectConfig
  -> CompiledProjectUpdate
  -> ReadyProjectUpdate
  -> PublishedRouteSnapshot
```

`commit()` 只接受 ready 类型；同版本忽略、空 Host 卸载、无效候选保旧等兼容语义保持
不变。

### 5.6 L-06：系统 DNS 配置和多 nameserver

**归属：双方。**

当前 access-server 同步读取 `/etc/resolv.conf`，只采用第一条有效 nameserver，失败时
回退到硬编码 `8.8.8.8`。Fiber `DnsClient::Options` 当前只持有一个 server，没有通用的
系统 resolver 配置或多上游 failover。

Fiber 上游应负责：

- 有界、可测试的系统 DNS 配置解析；
- nameserver 列表和轮转/failover；
- search/ndots 等能力应按明确范围逐步实现，不能静默部分支持；
- 配置 reload 是否支持及其线程/loop 所有权；
- 单元测试不得依赖宿主机真实 `/etc/resolv.conf`。

本项目负责：

- 提供显式 DNS server override 和系统配置选择策略；
- 在启动前读取/验证配置，不在请求 EventLoop 做文件 I/O；
- 将 Fiber resolver 实例化到每个 HTTP worker；
- 输出有界 DNS 健康和失败指标；
- 验证 Nacos、业务 upstream 和容器部署环境的实际行为。

## 6. 安全与信任边界

### 6.1 S-01：access log query 脱敏

**归属：本项目。Fiber 日志库无须修改。**

`AccessRequestTelemetry` 在 INFO 级记录 `unparsed_uri`，其中通常包含 query string，可能
泄露 token、签名 URL、session id 和业务敏感参数。

代码位置：[`AccessRequestTelemetry.cpp`](../src/observability/AccessRequestTelemetry.cpp#L269)。

建议默认只记录 path，并支持：

- query key allowlist；
- 敏感 key 固定替换；
- 可选的 query hash；
- 正常请求采样、失败请求保留；
- 最大字段长度和 UTF-8/控制字符安全编码。

Fiber 已经通过 `LoggerManager::queue_stats()` 和 appender stats 暴露队列、丢弃和写错误
统计。本项目只需将它们接入 access-server 的固定 schema 指标，不需要新增 Fiber API。

### 6.2 S-02：trusted proxy 和真实客户端地址

**归属：本项目。Fiber 已提供 socket peer 地址。**

当前 HTTPS 判断直接读取 `X-Forwarded-Proto`，CIDR 和 gray 策略直接读取 `X-Real-Ip`。
无可信代理边界时，客户端可以伪造这些 header。原始 IPv6 或非预期格式解析失败后还会按
Java 行为跳过 allow/deny。

代码位置：

- [`AccessRequestHandler.cpp`](../src/execution/AccessRequestHandler.cpp#L171)；
- [`AccessRequestHandler.cpp`](../src/execution/AccessRequestHandler.cpp#L189)；
- [`GrayMatchStore.cpp`](../src/runtime/GrayMatchStore.cpp#L54)。

Fiber `HttpExchange::remote_addr()` 已经提供真实 socket peer，因此本项目应实现
`ClientMetadataResolver`：

1. peer 属于 configured trusted-proxy CIDR 时，才解析转发 header；
2. 非可信 peer 使用 `remote_addr()` 和实际 TLS scheme；
3. 对 `Forwarded`、`X-Forwarded-For`、`X-Real-Ip` 的支持范围和优先级显式化；
4. 处理 IPv4、IPv6、带端口、括号、多值和非法值；
5. access policy、gray、日志和 trace 统一消费同一个解析结果；
6. Java 旧行为通过显式 compatibility mode 和 fixture 保留，不静默改变。

如果 listener 可被非受信网络直接访问，该项应视为 P0。

### 6.3 S-03：上游 TLS 验证

**归属：本项目。Fiber 已具备所需基础能力。**

当前 HTTPS upstream 明确设置 `verify_peer=false`，以匹配 Java 基线。Fiber
`TlsOptions` 和客户端 transport 已支持 `verify_peer`、`ca_file`、`server_name` 和
`verify_name`，服务端证书验证无需先改 Fiber。

代码位置：[`ProxyUpstreamConnection.cpp`](../src/execution/ProxyUpstreamConnection.cpp#L22)。

本项目应增加显式、可审计的模式：

- `legacy_insecure`；
- 系统 CA 验证；
- 指定 CA bundle；
- SNI 与独立证书验证名；
- 证书验证失败的稳定错误、CAT/metrics 结果和敏感信息脱敏。

不能直接改变已有路由默认语义。新增字段必须同步 native codec/runtime、validator、server、
web、fixture 和兼容文档。

### 6.4 S-04：上游 mTLS 客户端身份

**归属：双方，Fiber 为前置实现方。**

Pinned Fiber 的 `TlsOptions` 虽包含 `cert_file/key_file`，但 `TlsContext::init()` 当前只在
server context 中调用证书和私钥加载逻辑，client context 不会加载客户端身份。因此不能
仅通过 access-server 设置现有字段来宣称已经支持 upstream mTLS。

Fiber 上游负责：

- client context 加载证书链和私钥；
- 私钥匹配、证书格式、失败状态和敏感错误处理；
- 与 `verify_peer`、SNI、`verify_name` 和 ALPN 的组合测试；
- 明确证书 context 的复用、热更新和连接池 key 语义。

本项目负责：

- route/环境级 mTLS 配置模型和 secret 引用；
- 不回显私钥、证书 secret 和路径；
- 将客户端身份纳入 connection pool key，禁止不同身份错误复用连接；
- native codec、validator、server、web、fixture 和兼容文档；
- 与确定性 loopback TLS upstream 的端到端验证。

## 7. 请求热路径和性能

请求的主要执行链如下：

```text
atomic route snapshot pin
  -> Host/Path matcher
  -> gray snapshot + shared sample sequence
  -> service directory mutex + SWRR mutex + endpoint scan
  -> connection pool / DNS / connect
  -> template and header materialization
  -> CAT / metrics / access log
```

### 7.1 P-01：service selection 双层共享锁

**归属：双方。**

`AccessServiceState::Impl::select()` 在 service mutex 内查找 cluster，再调用带独立 mutex 的
`SmoothWeightedRoundRobin::select()`；完成或失败报告会再次获取 SWRR mutex。所有 HTTP
worker 访问同一 service 时会共享这些 cache line，选择本身还需要 O(N) 扫描 endpoint。

代码位置：

- [`AccessServiceDiscovery.cpp`](../src/runtime/AccessServiceDiscovery.cpp#L250)；
- [`SmoothWeightedRoundRobinImpl.h`](../src/runtime/SmoothWeightedRoundRobinImpl.h#L353)；
- [`SmoothWeightedRoundRobinImpl.h`](../src/runtime/SmoothWeightedRoundRobinImpl.h#L395)。

第一阶段由本项目完成：

- service/cluster 目录改成不可变 snapshot；
- selection 不再持有目录 mutex；
- 保留当前全局 SWRR 状态和锁，先消除双层锁；
- 建立 endpoint 数 1/8/32/128、worker 数 1 到 CPU 数的 contention benchmark。

第二阶段由双方协同：

- Fiber 上游承载通用 SWRR、selection token、update、complete 和 circuit 状态；
- 评估 sharded/per-loop balancer API，避免框架组件依赖 access-server 类型；
- 本项目保留 zone、cluster、gray、Java retry 次数和兼容错误映射；
- 本项目验证全局 SWRR 改成 per-worker 后的流量分布和故障恢复语义。

如果 benchmark 证明第一阶段已经足够，不应仅为代码复用强行迁移上游。

### 7.2 P-02：per-worker route snapshot 和请求 pin

**归属：双方，且只有 profile 证明需要时实施。**

每个请求都会 atomic load 一个 `shared_ptr<const AccessRouteSnapshot>`。该操作不保证
lock-free，并带来共享控制块引用计数流量，但它目前提供了清晰可靠的请求生命周期保证。

代码位置：

- [`RouteConfigStore.h`](../src/runtime/RouteConfigStore.h#L62)；
- [`AccessRequestHandler.cpp`](../src/execution/AccessRequestHandler.cpp#L320)。

候选方案是每 worker snapshot slot 加 epoch/RCU：更新通过 EventLoop fan-out，旧快照等
该 worker 的旧 generation 请求全部结束后回收。不能简单换成裸指针。

归属边界：

- Fiber：若实现通用 loop-local immutable snapshot、epoch 或 RCU 原语，其生命周期、取消、
  loop shutdown 和内存顺序属于框架；
- 本项目：route generation、worker acknowledgement、请求 pin 和 activation evidence；
- 双方：TSAN、并发热更新、请求跨 await、shutdown 中更新等回归。

在没有 profile 前保持当前 shared pointer 实现。

### 7.3 P-03：per-worker gray snapshot 和 PRNG

**归属：本项目。**

gray 判断每次 atomic load 共享 snapshot，并通过全局 atomic sequence 产生采样值。gray
随机序列不是外部精确兼容契约，可以改为每 worker snapshot 和 PRNG，消除共享 cache
line。

代码位置：[`GrayMatchStore.cpp`](../src/runtime/GrayMatchStore.cpp#L108)。

需要验证 ratio 分布、更新 generation、固定 seed 测试和 worker 数变化，不得改变未知
entry、无效 CIDR、ratio 边界等既有兼容行为。

### 7.4 P-04：TLS hazard 回收调度

**归属：本项目。**

TLS identity selector 使用 hazard pointer，避免握手热路径 shared ownership，这是合理的
设计。但每次清除 hazard 都调用 `request_reclaim()`；即使没有 retired snapshot，也可能
向 Nacos owner loop 投递 reaper。

代码位置：[`TlsCertificateStore.cpp`](../src/runtime/TlsCertificateStore.cpp#L473)。

建议：

- 维护 `retirement_pending`；
- 只有 rotation 后存在未回收 snapshot 才允许投递；
- publish 时先主动 reclaim，仍被 worker hazard 持有才等待后续 clear；
- 记录 rotation/reclaim 次数、retired 数量和最长保留时间；
- 在没有 hazard-pointer 内存模型证明前不放松 `seq_cst`。

Fiber 的 TLS selector 已能提供 ClientHello 和 context 选择；当前问题来自本项目的动态
证书快照实现，不要求 Fiber 修改。若未来多个应用都需要动态 TLS identity store，再单独
评估是否上游化，不能与本次修复绑定。

### 7.5 P-05：模板、URI 和 header 分配

**归属：本项目。**

明确的请求级分配包括：

- `evaluate_template()` 只按 literal 大小 reserve，并为每个 expression 创建临时 string；
- 无 rewrite 时仍复制完整 request target；
- 每请求构造 `project + ".unifiedAccess"`；
- response/proxy header plan 再次复制已编译 header 名称和值。

代码位置：

- [`TemplateEvaluator.cpp`](../src/execution/TemplateEvaluator.cpp#L7)；
- [`ProxyExecutor.cpp`](../src/execution/ProxyExecutor.cpp#L222)；
- [`ProxyExecutor.cpp`](../src/execution/ProxyExecutor.cpp#L394)；
- [`ResponsePlan.cpp`](../src/execution/ResponsePlan.cpp#L247)；
- [`ProxyResponsePlan.cpp`](../src/execution/ProxyResponsePlan.cpp#L21)。

建议：

- script template 增加 append-to-sink/request-pool builder；
- request target 使用 `string_view + optional owned buffer`；
- call-source 在 project snapshot 编译时预计算；
- 静态 header name 保留 view，动态 value 使用 request pool；
- 用 allocation/request、bytes/request 和 p99 验证收益。

这些行为包含 Java 模板和 header 兼容逻辑，应留在 access-server；无需修改 Fiber 通用
HTTP API，除非实际实现发现 request-pool header 接口缺少必要的安全生命周期能力。

### 7.6 P-06：初始项目批量发布

**归属：本项目。**

每个项目 commit 都复制全部 project snapshot 并重建全局 Host matcher；全局 snapshot
构建还用嵌套循环检查重复项目。数百项目逐个到达时，会反复处理越来越大的项目集合。

代码位置：

- [`RouteConfigStore.cpp`](../src/runtime/RouteConfigStore.cpp#L99)；
- [`AccessRouteSnapshot.cpp`](../src/routing/AccessRouteSnapshot.cpp#L18)。

建议：

- owner-loop registry 通过 map/sorted index 保证 project 唯一；
- 初始同步期间 batch/coalesce 已完成准备的项目；
- 一次构建全局 Host matcher 后发布；
- 正常热更新仍保留按候选原子发布和失败保旧；
- 分别记录单项目 compile、service ready、global build 和 publish 时间。

### 7.7 P-07：Host matcher 高 fan-out

**归属：本项目，profile 驱动。**

`HostMatcher::find_child()` 对当前节点的 children 做线性扫描。通常域名层数和同级 label
数量都很小，因此优先级低于 service lock 和分配。

代码位置：[`HostMatcher.cpp`](../src/routing/HostMatcher.cpp#L165)。

若真实配置显示高 sibling fan-out，可在 build 完成后排序并使用二分查找，或采用紧凑
哈希索引。必须保留 exact child 阻止祖先 wildcard fallback 等 Java 特殊语义。

### 7.8 P-08：Happy Eyeballs

**归属：双方，Fiber 为前置实现方。**

当前 proxy 按 DNS 地址顺序串行 connect。首个 IPv6/IPv4 地址不可达时，尾延迟可能叠加
多个完整 connect timeout。

代码位置：[`ProxyUpstreamConnection.cpp`](../src/execution/ProxyUpstreamConnection.cpp#L39)。

Fiber 上游应实现可复用的异步多地址 connector：

- RFC 8305 风格的地址交错和 stagger delay；
- 并发拨号上限；
- 首个成功者获胜，其余任务可取消并安全关闭；
- EventLoop 所有权、timeout 和 shutdown 明确；
- 返回稳定、可诊断但不泄露敏感地址的错误摘要；
- 为 HTTP/其他协议复用，不依赖 access-server route 类型。

本项目负责：

- 使用该 connector 组装 `Http1ClientConnection`；
- 保持 connection pool lease 和 Java 最大尝试次数；
- 配置是否启用、stagger/timeout 上限；
- DNS、connect、selection report、CAT 和 metrics 映射；
- loopback IPv4/IPv6、取消、downstream close、pool shutdown 回归。

## 8. 类职责优化

### 8.1 C-01：`AccessServerRuntime`

**归属：本项目。**

当前类同时构造 Nacos/CAT、脚本 runtime、service discovery、三个 watcher、route/gray/TLS
store、业务 server，并管理三个 loop 的启动、失败回滚和逆序关停。它作为 composition
root 合理，但具体依赖过多，生命周期难以使用 fake 完整测试。

建议拆为：

- `RuntimeFactory`：生产依赖构造；
- `ControlPlaneSupervisor`：Nacos、watcher、discovery、TLS；
- `DataPlaneService`：业务 listener、worker resources、metrics listener；
- `RuntimeCoordinator`：小型显式状态机。

避免为每个依赖引入虚基类；优先使用小 adapter、factory 和可注入的 concrete test fake。

### 8.2 C-01：`AccessServer`

**归属：本项目。**

建议保留其 data-plane façade 角色，但将 DNS、pool、CAT worker attachment 抽成
`WorkerResources`，metrics listener 抽成 `MetricsEndpoint`。`AccessServer` 最终只负责
initialize、bind、serve 和全异步 shutdown。

### 8.3 C-01：配置 watcher

**归属：本项目。**

三个 watcher 重复维护订阅状态、initial ready、closed、stop、update counters 和最后
失败。建议用组合式 `SubscriptionLifecycle` 统一 generation、stale callback、retry、
ready 和 failure event；资源特有的 decode/prepare/commit 仍放在各 watcher，避免复杂
继承树。

### 8.4 C-01：`RouteConfigStore` 和 `ProjectRouteSnapshot`

**归属：本项目。**

建议拆分为：

- `ProjectConfigCompiler`；
- `ProjectSnapshotRegistry`；
- `RouteSnapshotPublisher`；
- 只包含不可变数据和查询方法的 `ProjectRouteSnapshot`。

`AccessServiceSelectorFactory::begin_compile()/take_error()` 的隐式错误侧信道应改为明确的
result，方便异步编译、重入和测试。

### 8.5 C-01：`AccessRequestHandler`

**归属：本项目。**

Handler 当前直线式 pipeline 比较清晰，不建议过度拆分。只抽出：

- `ClientMetadataResolver`：peer/trusted proxy/scheme/source address；
- `RoutePolicyEvaluator`：entry、HTTPS、body、CIDR 纯策略。

Handler 继续负责 pin、匹配、准备 request context 和选择 executor。

### 8.6 C-01：`ProxyExecutor`

**归属：本项目。**

当前单个大 coroutine 同时处理 request plan、gray、选址、retry、pool/DNS/connect、CAT、
body、response rewrite 和 WebSocket。建议分为：

- `ProxyRequestPlan`；
- `UpstreamAttempt`；
- `HttpResponseBridge`；
- `WebSocketBridge`。

优先拆纯同步规划和独立 bridge，不用 `std::function` 或虚调用把每一步插件化。拆分前后
比较 coroutine frame、二进制 text、吞吐和 p99，防止只改善文件大小而损害热路径。

### 8.7 C-01：`AccessRequestTelemetry`

**归属：本项目。**

保留一个 request-lifetime façade，内部拆成 `TracePropagation`、`RequestObservability` 和
`ScriptExecutionContext`。这样可以分别测试 CAT 禁用/失败、W3C trace、header 注入、日志
和指标，同时不分散请求状态的生命周期所有权。

### 8.8 C-01：`AccessConfigCodec`

**归属：本项目。**

先按通用 scalar coercion、project/route、gray、field-path error builder 分区。不要为了
缩短文件直接采用反射式 codec，也不要在没有内存 profile 时重写成流式 parser；Java
标量转换、duplicate/unknown/null 和错误 offset 属于兼容契约。

## 9. 可观测性和实例激活证据

### 9.1 O-01：实例级激活证据

**归属：本项目，涉及 native、server 和 web。**

当前 watcher 的成功/失败计数和最后错误大多只存在内存，业务进程没有对控制面提供类型化
实例证据。rnacos write/readback 只能证明发布结果，不能证明某个实例已经编译并使用该
版本。

建议提供经过认证、有界、分页的实例状态协议，至少包含：

- instance ID、build revision、启动时间；
- resource kind、Data ID、group、observed MD5/version；
- decode、compile、service-ready、publish result；
- 当前 global snapshot generation/fingerprint；
- 采用 per-worker snapshot 后的 worker acknowledgement；
- TLS snapshot version 和 certificate count；
- discovery ready 和有界聚合数量；
- 脱敏的最后失败及发生时间。

高基数明细不能进入 Prometheus label，也不应放在当前无认证的 metrics endpoint。server
负责认证、采集、持久化和环境隔离；web 必须继续区分 draft、published 和 instance
activation。在协议实现前 activation 保持 `unknown`。

### 9.2 O-02：有界运行指标

**归属：本项目。Fiber 已提供部分原始统计。**

建议增加：

- config update result：resource kind + bounded reason；
- Nacos config/naming connected 和 subscription state；
- active project/host/route 数量和 snapshot age；
- service/endpoint 聚合数量；
- proxy phase/outcome/attempt、pool hit、DNS outcome；
- TLS rotation/reclaim；
- WebSocket outcome；
- async log queue/appender drop 和 CAT drop。

project、route、cluster、host、Data ID、service name、header 和用户数据不得成为无界 label。
Fiber 的 pool lease、LoggerManager queue stats 等已有接口应直接消费；只有确实缺少通用、
有界统计时才提交上游 API。

## 10. 构建边界

### 10.1 C-02：拆分 `access_server_core`

**归属：本项目。**

当前一个 static library 同时包含 config、routing、execution、observability、runtime 和
validation，并 PUBLIC 依赖 Nacos、CAT、Prometheus。离线 validator 因此也只有在完整
runtime 组件启用时才能构建。

建议拆分为：

```text
access_server_config
  -> codec, compiled model, route compiler, script compiler adapter

access_server_execution
  -> handler, response, proxy, request telemetry facade

access_server_observability
  -> metrics, trace and logging integration

access_server_runtime
  -> Nacos, CAT, discovery, watcher, DNS, listeners

access_server_validation
  -> native validator protocol and orchestration
```

validator 只依赖 config/compiler/validation；runtime 才依赖 Nacos/CAT/Prometheus。源文件
继续显式列出，不使用 glob。PUBLIC/PRIVATE link interface 应按头文件实际暴露收紧。

## 11. 测试与性能基线

### 11.1 T-01：正确性和并发测试

**归属：本项目；涉及 Fiber 改动时同时运行上游测试。**

应优先增加：

- `AccessServerRuntime` 每个启动阶段的失败和逆序回滚；
- signal 到达 startup 中间阶段、重复 shutdown；
- DNS 部分初始化、worker 停止和异步释放；
- project subscribe 失败、closed、retry、stale generation；
- 初始 project-list 与 route/service readiness 的各种到达顺序；
- TLS rotation 与握手并发、retired snapshot 回收；
- route snapshot 更新与跨 await 请求 pin；
- trusted proxy、IPv6、多值和非法 forwarding header；
- 上游 TLS 验证成功、未知 CA、名称错误和客户端证书；
- ASAN/UBSAN、聚焦 TSAN；
- codec、Host/CIDR、trace state、validator fuzz。

Fiber DNS、connector、SWRR 或 RCU 有上游改动时，必须在 Fiber 仓库先补独立测试，再更新
gitlink 并运行完整 Fiber/native 回归。

### 11.2 T-02：benchmark

**归属：双方，各自测试自己的抽象。**

本项目 benchmark：

- route pin + Host/Path 匹配；
- template/header plan；
- gray sampling；
- service selection 在多 worker 下的竞争；
- TLS identity select/reclaim；
- 352 项目 burst compile/build/publish；
- loopback HTTP proxy、连接复用和 WebSocket；
- CAT/logging 开关与采样的开销。

Fiber benchmark：

- 通用 SWRR update/select/complete；
- multi-nameserver DNS/failover；
- Happy Eyeballs 不同地址和失败组合；
- 若引入 RCU，则测试读取、更新、回收和 shutdown。

至少记录 throughput、CPU/core、p50/p95/p99、allocations/request、bytes/request、锁竞争、
连接池 hit ratio、配置更新时间和旧快照最长保留时间。

## 12. 文档与兼容门禁

### 12.1 D-01：生产 corpus 状态

**归属：本项目。**

`script-corpus-differential.md` 记录当前脱敏快照 352/352 完成 decode 和 compiled snapshot
构建；README 和 compatibility contract 仍说明完整生产 corpus、阶段 8 和切流门禁未完成。
二者不一定矛盾，但表述容易被理解成同一个 gate。

建议每份差分记录包含：

- corpus 获取日期、脱敏版本和 SHA-256；
- 项目/route/script/template 数量；
- compile-only、Java golden、请求级 differential 分别完成多少；
- 未覆盖能力和已知差异；
- 是否满足当次阶段 gate；
- 最终切流 gate 明确保持未完成，直到所有要求实际通过。

## 13. Fiber 能力核对结果

为了避免不必要的上游修改，本次已核对 pinned Fiber API：

| 能力 | 当前 Fiber 状态 | 结论 |
| --- | --- | --- |
| HTTP socket peer 地址 | `HttpExchange::remote_addr()` 已提供 | trusted proxy 在本项目实现 |
| TLS peer/CA/SNI/验证名 | client context 已支持 | 上游 TLS 安全模式在本项目接入 |
| TLS 客户端证书 | 字段存在，但 client context 当前不加载身份 | Fiber 补能力，本项目接入 mTLS |
| 异步日志丢弃统计 | LoggerManager/Appender stats 已提供 | 本项目接 Prometheus |
| EventLoop worker index | `group_index()` 已提供 | DNS resolver 可直接 O(1) 取 slot |
| connection pool 异步 shutdown | `shutdown_async()` 已提供 | 保持使用 |
| DNS resolver | 单 server、cache、A/AAAA policy 已提供 | 系统配置和多 server 需上游增强 |
| Happy Eyeballs | 未发现通用实现 | Fiber 实现，本项目接入 |
| 通用 SWRR | 当前实现位于 access-server | 稳定后考虑上游化 |
| 通用 loop-local RCU | 当前未作为公共能力使用 | 仅在 profile 证明需要时设计 |

## 14. 推荐落地顺序

### 阶段 A：先修确定性问题

1. L-01 DNS shutdown 全异步化；
2. L-02 项目订阅 retry/readiness；
3. S-01 access log query 脱敏；
4. S-02 trusted proxy 和真实客户端地址；
5. L-03 配置上限及 runtime/validator/Console 一致性；
6. O-01/O-02 先暴露脱敏的失败、readiness 和固定维度指标；
7. 补相应生命周期和安全测试。

以上全部可在本项目独立完成，不需要等待 Fiber。

### 阶段 B：建立性能基线后处理共享热点

1. 建立 T-02 benchmark；
2. P-04 修复 TLS reaper 调度；
3. P-01 先消除 service directory 外层锁；
4. P-03 per-worker gray snapshot/PRNG；
5. P-05 处理已确认的字符串/header 分配；
6. P-06 初始项目 batch build/publish；
7. L-04 配置编译移出 Nacos loop；
8. L-05 收敛 prepared/ready/published API；
9. S-03 接入上游 peer/CA/SNI 验证模式。

### 阶段 C：Fiber 协同能力

1. L-06 系统 DNS 配置和多 nameserver；
2. P-08 Happy Eyeballs；
3. S-04 upstream mTLS client identity；
4. P-01 通用 SWRR 上游化或 sharding；
5. 只有 profile 证明 route pin 成为瓶颈时，开展 P-02 RCU 设计。

每项先在 Fiber 上游合入并测试，再更新本仓库 gitlink 和 provenance，最后运行完整
Fiber/native 回归。

### 阶段 D：结构收敛和最终门禁

1. C-01 按已稳定的运行边界拆类；
2. C-02 拆 CMake targets；
3. 完成 sanitizer、fuzz、故障注入和压力测试；
4. 完成生产 corpus 和阶段 8 全量差分；
5. 基于实例证据验收 published 与 activation 状态；
6. 达到所有门禁前继续明确标记“不满足生产切流条件”。

## 15. 不应作为“优化”破坏的语义

以下行为属于兼容或正确性边界，任何性能和结构调整都必须保留或显式变更契约：

- 同 version route candidate 忽略；
- 无效 candidate 保留旧 snapshot；
- Host 为空的项目卸载与项目从列表移除不是同一语义；
- exact Host child 与 wildcard fallback 的 Java 特殊行为；
- request 必须在整个异步执行期间 pin 住同一配置；
- proxy pool lease、service selection generation 和 WebSocket 生命周期；
- header/body 原子准备和 protected-header 过滤；
- gray runtime 宽松兼容与 Console validator 严格 authoring 的区别；
- draft、rnacos published、instance activation 三种状态不得合并；
- 指标 label 必须保持有界；
- 未完成 corpus differential 和阶段 8 前不得宣称生产兼容完成。
