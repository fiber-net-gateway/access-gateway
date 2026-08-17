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
| S-03 | P1 | 上游 TLS peer/CA/SNI 验证配置 | 双方 | 路由级配置需要 Fiber #28；进程级可先落地 |
| S-04 | P1 | 上游 mTLS 客户端身份 | 双方 | 是 |
| P-01 | P1 | 消除 service selection 双层共享锁 | 双方 | 通用 SWRR 上游化时需要 |
| P-02 | P1/P2 | per-worker route snapshot/RCU pin | 双方 | 采用通用原语时需要 |
| P-03 | P1 | per-worker gray snapshot 和 PRNG | 本项目 | 否 |
| P-04 | P1 | TLS hazard reaper 只在有退休对象时调度 | 本项目 | 否 |
| P-05 | P1 | 模板、request target、header 分配优化 | 本项目 | 否 |
| P-06 | P1 | 初始项目批量发布和全局 matcher 重建优化 | 本项目 | 否 |
| P-07 | P2 | Host matcher 高 fan-out 搜索优化 | 本项目 | 否 |
| P-08 | P1 | Happy Eyeballs/交错多地址连接 | 双方 | 是 |
| O-01 | P0/P1 | 实例级配置激活证据 | 本项目（已解决） | 否 |
| O-02 | P1 | 配置、发现、DNS、pool、proxy、TLS、日志指标 | 双方 | 是，Nacos 连接证据需 Fiber #27 |
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
- **S-03（路由级）**：connection pool key 纳入有界 TLS transport profile，避免不同
  CA/SNI/验证名复用同一连接；见
  [fiber-gateway-cpp #28](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/28)；
- **P-08**：可取消、可复用的 Happy Eyeballs 多地址 connector；
- **O-02**：暴露 Nacos config/naming transport、认证和 reconnect 的 typed bounded
  snapshot/watch；见 [fiber-gateway-cpp #27](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/27)。

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

**实施状态：已解决（2026-08-16）。** Watcher 现在保留所有 desired project entry，按错误
类型区分可重试和永久订阅失败，并以封顶指数退避重试暂态错误。每个 entry 显式记录订阅
状态、当前订阅首值、配置处理结果、observed md5/version、published generation、失败阶段和
下一次重试时间；删除项目和 shutdown 会取消等待中的 retry。启动门闩消费 typed readiness，
只有 project-list 首值及全部当前项目订阅首值处理完成后才继续，而且总超时预算不会被中间
状态更新重置。

这里的 `Ready` 表示本实例已经完整观察并处理当前订阅图，不等同于配置 activation。首个
非法候选会进入可观测的 `Rejected` 终态、保留旧快照或空快照，并计入 readiness 的 rejected
数量；它不会被报告成 published/active。实例级 activation evidence 仍由 O-01 单独实现。

改造前，`AccessConfigWatcher::apply_project_list()` 在完成项目列表 reconcile 后立即发布
ready。单项目 `subscribe()` 失败时，`add_project()` 只删除 entry 并增加 `failed_updates_`，
既不 retry，也不记录包含 Data ID 和错误原因的 `last_failure_`。

代码位置：

- [`AccessConfigWatcher.cpp`](../src/runtime/AccessConfigWatcher.cpp#L150)；
- [`AccessConfigWatcher.cpp`](../src/runtime/AccessConfigWatcher.cpp#L270)；
- [`AccessServerRuntime.cpp`](../src/runtime/AccessServerRuntime.cpp#L338)。

实例可能已经绑定 listener，但部分项目从未建立订阅。请求仍然 fail closed，但运维上
会产生“实例已就绪”的错误判断。

实施后的每项目状态包括：

- `desired`；
- `subscribing/subscribed/retrying/failed/retiring`；
- `first_value_received`；
- `observed_md5` 和 route version；
- 最新候选的 `awaiting/processing/accepted/rejected` 结果，以及失败发生在
  `subscription/decode/compile/service-ready/publish` 的哪个阶段；
- `published_generation`；
- `last_error` 和 `next_retry_at`。

初始订阅失败采用延迟封顶的指数退避。typed readiness 同时为后续运维接口保留以下边界：

- liveness：进程和 EventLoop 正常；
- control-plane connected：Nacos config/naming 已连接；
- readiness：要求的初始资源已经达到明确条件；
- activation：本实例已经发布某个编译快照。

本实现选择在全部 desired project 的当前订阅首值进入明确终态前不绑定 socket；Rejected
也是明确但非激活的终态，因此不会阻塞其他合法项目提供服务。readiness 不再等同于“看到了
project-list 首值”。

### 5.3 L-03：配置资源上限

**归属：本项目，且必须同步 native runtime、validator、server 和 web。**

**实施状态：已解决（2026-08-16）。** `AccessConfigLimits` 现在是 native runtime 和离线
Validator 的固定、版本化 source of truth。raw payload 在 JSON tokenization 前检查，codec
检查 container/string，snapshot compiler 检查 path variable、template expression、compiled
program、静态 body 和近似内存预算。Project route、Project List 和 gray 候选超限都返回稳定的
`LimitExceeded` 并保留旧状态；非法 Project List 不再触发订阅 reconcile 或项目卸载。

Validator 的 `--describe-config-limits` 输出 strict schema version 1 JSON。server 启动时探测并
校验该 schema，draft/compiler/Release 使用探测结果，`/api/system/status` 向 web 提供同一份限制；
编辑器展示 route/source/CIDR 配额并在明显超限时阻止保存。发布要求可用的 Validator revision
及其探测限制，不能用 server fallback 推断发布安全或实例激活。

代码位置：

- [`AccessConfigCodec.cpp`](../src/config/AccessConfigCodec.cpp#L969)；
- [`ProjectRouteSnapshot.cpp`](../src/routing/ProjectRouteSnapshot.cpp#L785)；
- [`TlsCertificateConfig.h`](../src/config/TlsCertificateConfig.h#L13)；
- [`NativeValidatorProtocol.h`](../src/validation/NativeValidatorProtocol.h#L10)。
- [`config-resource-limits.md`](config-resource-limits.md)。

共享的 `AccessConfigLimits` 包含：

- project-list 总字节、项目数和项目名长度；
- 单项目 route snapshot 原始字节；
- host、route、header、CIDR、address 数量；
- path、method、condition、script、template、header name/value 长度；
- 单响应 identity/gzip body 和整个项目静态响应总字节；
- 最大 path variable、template expression 和 compiled program 数量；
- 编译后 snapshot 的近似内存预算。

超限必须返回稳定的 `LimitExceeded`、字段路径和脱敏消息，保留旧快照。Console validator
和运行时引用同一份 native 限制，避免控制面接受数据面必然拒绝的内容。具体 schema version 1
数值和 snapshot 估算边界见上面的独立限制文档。

### 5.4 L-04：配置编译移出 Nacos owner loop

**归属：本项目。初始实现不需要 Fiber 改动。**

**实施状态：已解决（2026-08-17）。** runtime 现在使用独立单线程 compiler EventLoop。
Project route 的 JSON、关系、脚本/模板、CIDR/address、matcher 和静态 gzip，以及 TLS 的
JSON、PEM/SAN、TCP/QUIC context 和 bootstrap identity 准备都在该 loop 完成。Nacos loop
只检查原始字节上限、推进 generation、绑定 owner-loop-only NamingService lease、等待
service ready 并发布完整候选。

Project 队列对每个当前项目只保留 latest `ConfigData` shared pointer，实际投递任务为 1，
总待处理项目受 L-03 的 1024 项目上限约束；TLS 使用 1 active + 1 latest pending。旧任务
通过 cancel flag 和回投后的 generation 双重检查失效。shutdown 会先关闭订阅、取消候选并
异步等待全部回执，再允许 compiler group 停止。完整线程、队列和生命周期契约见
[`config-compilation.md`](config-compilation.md)。

改造前，项目回调在 Nacos loop 上完成 JSON decode、关系校验、脚本和模板编译、
CIDR/address 编译以及静态 gzip；TLS watcher 也会在 owner loop 上解析 PEM 和创建 TLS
context。大配置或复杂脚本会延迟同一 loop 上的其他配置和 NamingService 工作。

采用的执行模型是：

```text
Nacos owner loop
  -> 检查原始字节上限并记录 generation
  -> compiler worker 纯 decode/validate/compile
  -> 回到 Nacos loop 检查 generation
  -> 获取 service lease 并等待 ready
  -> 原子发布
```

脚本 compiler 在 compiler loop 内拥有独立、长生命周期的 `AccessScriptCompiler`；OpenSSL
context 也只在该 worker 构造。队列容量、取消和“新 generation 替换旧任务”均由 watcher
显式管理。

若未来需要通用 CPU work executor，可单独贡献到 Fiber；本项不应因等待通用抽象而阻塞
本项目的专用实现。

### 5.5 L-05：配置发布 typestate

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** `RouteConfigStore` 现在以不可复制、不可默认构造的
`PreparedProjectUpdate` 和 `ReadyProjectUpdate` 表达发布状态。前者只能通过逐 selector 的
`try_ready() &&` 或 `wait_ready() &&` 单向转换为后者，`commit()` 只接受 Ready 类型；字段均为
private，move 会转移有效性标记，未 ready、伪造或重复消费候选无法进入正常提交路径。
`prepare_compiled()` 还会在新版本候选进入 Prepared 前校验 snapshot 内嵌 project/version
与请求一致；同版本候选仍按兼容规则直接忽略。

watcher 把 Prepared 所有权移入 readiness 协程，与 Project revision 竞速；新 generation
获胜会销毁候选并释放 NamingService lease。ready 成功后仍重新检查 watcher、Project identity
和 generation，再统一经 `commit_ready_project()` 更新 snapshot、计数、observer 和
`published_generation`。同步 `apply()` 也必须先通过 `try_ready()`，不再拥有绕过路径。

完整类型不变量、转换、状态表及取消语义见
[`config-publication-typestate.md`](config-publication-typestate.md)。改造前，
`ProjectRouteSnapshot::wait_ready()` 是异步 readiness，而 Nacos selector 的
`ready_for_publish()` 恒为 false；watcher 等待成功后直接调用接受 Prepared 的 `commit()`，
容易被新调用方误用。

代码位置：

- [`ProjectRouteSnapshot.cpp`](../src/routing/ProjectRouteSnapshot.cpp#L754)；
- [`AccessServiceDiscovery.cpp`](../src/runtime/AccessServiceDiscovery.cpp#L351)；
- [`RouteConfigStore.cpp`](../src/runtime/RouteConfigStore.cpp#L84)。

实现后的状态转换为：

```text
ProjectConfig (parsed)
  -> CompiledProjectConfig
  -> PreparedProjectUpdate
  -> ReadyProjectUpdate
  -> PublishedRouteSnapshot
```

同版本忽略、空 Host 卸载、无效候选保旧等兼容语义保持不变。本项没有修改 Fiber 或 TLS
store；TLS Prepared 候选没有外部异步 readiness，且 commit 会在 owner loop 重判版本/digest。

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

**实施状态：已解决（2026-08-16）。** `AccessRequestTelemetry` 不再把 `unparsed_uri`
写入 INFO 日志，而是消费独立 `AccessLogPolicy` 生成的有界安全字段。默认 query allowlist
为空，只记录 path；内置敏感 key 不可被配置移除，命中 allowlist 时固定替换为
`[REDACTED]`。配置还支持附加敏感 key、实例随机密钥 HMAC-SHA256、成功请求 bps 采样及
path/query 字节上限。4xx/5xx、执行失败、未完成响应和 IO 错误始终保留。

所有 path/query 输出只包含安全 ASCII，控制字符、非 ASCII 和非法 UTF-8 字节按 `%XX`
编码，截断不会切断编码单元；日志显式给出 filtered/redacted/truncated/hash-failed 状态。
HMAC 只对最终保留的日志计算，采样使用 worker-local 无锁序列，不在请求热路径引入共享
计数器竞争。启用 HMAC 后安全随机密钥初始化失败会使 server 初始化失败。

改造前，`AccessRequestTelemetry` 在 INFO 级记录 `unparsed_uri`，其中通常包含 query
string，可能泄露 token、签名 URL、session id 和业务敏感参数。

代码位置：[`AccessRequestTelemetry.cpp`](../src/observability/AccessRequestTelemetry.cpp#L269)。

实现现在默认只记录 path，并支持：

- query key allowlist；
- 敏感 key 固定替换；
- 可选的 query hash；
- 正常请求采样、失败请求保留；
- 最大字段长度和 UTF-8/控制字符安全编码。

Fiber 已经通过 `LoggerManager::queue_stats()` 和 appender stats 暴露队列、丢弃和写错误
统计。本项目只需将它们接入 access-server 的固定 schema 指标，不需要新增 Fiber API。

### 6.2 S-02：trusted proxy 和真实客户端地址

**归属：本项目。Fiber 已提供 socket peer 地址。**

**实施状态：已解决（2026-08-16）。** 新增无请求期容器分配的 `ClientMetadataResolver`，
提供安全默认 `direct`、可信链 `trusted_proxy` 和显式兼容 `legacy_headers` 三种模式。可信链只在
socket peer 命中启动时严格编译的 CIDR 后生效，按 `Forwarded`、`X-Forwarded-For`、
`X-Real-Ip` 优先级解析并从右向左剥离可信 hop；IPv4、IPv6、括号、端口、多字段、最多 32 hop
和 XFP 下标对齐均有覆盖。非法高优先级输入不向低优先级 header 降级，而是使用 socket peer 和
listener 的真实 TLS 状态。

解析结果由 route CIDR、gray、HTTPS redirect、代理 Location/Refresh、CAT 和 access log 共同消费；
安全模式不再因 header 缺失或解析失败跳过 CIDR。`legacy_headers` 单独保存 route/gray 的 Java
兼容 target，继续覆盖旧 fixture，但必须显式配置。Fiber HTTP/1 exchange 当前不携带 scheme，
本项目利用业务 listener 全局 TLS 配置确定真实协议，因此本项不需要修改 Fiber。

改造前 HTTPS 判断直接读取 `X-Forwarded-Proto`，CIDR 和 gray 策略直接读取 `X-Real-Ip`。
无可信代理边界时，客户端可以伪造这些 header。原始 IPv6 或非预期格式解析失败后还会按
Java 行为跳过 allow/deny。

代码位置：

- [`ClientMetadata.h`](../src/execution/ClientMetadata.h)；
- [`ClientMetadata.cpp`](../src/execution/ClientMetadata.cpp)；
- [`AccessRequestTelemetry.cpp`](../src/observability/AccessRequestTelemetry.cpp)；
- [`GrayMatchStore.cpp`](../src/runtime/GrayMatchStore.cpp)。

实现遵循以下契约：

1. peer 属于 configured trusted-proxy CIDR 时，才解析转发 header；
2. 非可信 peer 使用 `remote_addr()` 和实际 TLS scheme；
3. 对 `Forwarded`、`X-Forwarded-For`、`X-Real-Ip` 的支持范围和优先级显式化；
4. 处理 IPv4、IPv6、带端口、括号、多值和非法值；
5. access policy、gray、日志和 trace 统一消费同一个解析结果；
6. Java 旧行为通过显式 compatibility mode 和 fixture 保留，不静默改变。

如果 listener 可被非受信网络直接访问，该项应视为 P0。

### 6.3 S-03：上游 TLS 验证

**归属：双方。进程级策略由本项目完成；路由级策略依赖 Fiber #28。**

改造前 HTTPS upstream 固定设置 `verify_peer=false`，以匹配 Java 基线。Fiber
`TlsOptions` 和客户端 transport 已支持 `verify_peer`、`ca_file`、`server_name` 和
`verify_name`，服务端证书验证无需先改 Fiber。

代码位置：[`UpstreamTlsClientPolicy.cpp`](../src/execution/UpstreamTlsClientPolicy.cpp)、
[`ProxyUpstreamConnection.cpp`](../src/execution/ProxyUpstreamConnection.cpp) 和
[`AccessServerRuntime.cpp`](../src/runtime/AccessServerRuntime.cpp)。

本项目现已增加显式、可审计的进程级模式：

- `legacy_insecure`；
- `system_ca` 系统 CA 验证；
- `custom_ca` 指定 CA bundle；
- hostname 自动作为 SNI/验证名，IP endpoint 只设置 IP `verify_name`、不发送 IP SNI；
- trust store 启动前 fail-fast；
- 可识别验证/ALPN 失败使用 502 `HTTP_CLIENT_TLS_ERROR` 和 CAT `tls` phase，且不回显
  CA 路径。

配置通过 `ACCESS_SERVER_UPSTREAM_TLS_MODE` 和
`ACCESS_SERVER_UPSTREAM_TLS_CA_FILE` 提供，默认仍为 `legacy_insecure`，因此不改变已有
route wire 或 Java 兼容默认。策略在进程生命周期内不可变，同一进程只有一个 trust profile，
不会触发当前 endpoint-only pool key 的跨策略复用。

完整的 route/环境级 CA、SNI 和独立验证名仍不能安全发布：Fiber 的
`Http1ConnectionGroupKey` 当前只包含 endpoint 与 scheme，pool hit 会绕过新连接 options，可能
复用由其他 TLS profile 创建的连接。该前置能力由
[fiber-gateway-cpp #28](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/28) 跟踪。
在上游完成前，native codec、validator、server 和 web 均不接受或模拟 route 级字段；之后新增
wire 字段时必须同步 codec/runtime、validator、server、web、fixture 和兼容文档。

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
  -> per-worker gray snapshot + loop-local sample sequence
  -> atomic service directory pin + SWRR mutex + endpoint scan
  -> connection pool / DNS / connect
  -> template and header materialization
  -> CAT / metrics / access log
```

### 7.1 P-01：service selection 双层共享锁

**归属：双方。**

**实施状态：第一阶段已解决（2026-08-17）。** service/cluster 映射现在由 Nacos owner
EventLoop 构建为按 cluster 名排序的不可变目录，并通过 atomic `shared_ptr` release/acquire
发布。请求选择只 pin 一次目录、二分查找 cluster，再进入该 cluster 的 SWRR；不再持有
service mutex，也不会把目录锁带入 endpoint 扫描。更新继续复用同名 cluster 的 SWRR，因而
保留 selection token、权重进度和 circuit state；删除或 retire 先发布新目录/空目录，旧目录
由已经开始的 `select()` 固定并自然回收。更新 bookkeeping 和 discovery metrics 保持 owner-loop
私有。

本阶段没有修改 Fiber，也没有把 SWRR 算法迁移到上游。atomic `shared_ptr` 的引用计数并不
保证 lock-free，而全局 SWRR 的 mutex 和 O(N) endpoint 扫描仍然存在；是否开展第二阶段必须
由下面的 benchmark 和生产 profile 决定。

改造前，`AccessServiceState::Impl::select()` 在 service mutex 内查找 cluster，再调用带独立 mutex 的
`SmoothWeightedRoundRobin::select()`；完成或失败报告会再次获取 SWRR mutex。所有 HTTP
worker 访问同一 service 时会共享这些 cache line，选择本身还需要 O(N) 扫描 endpoint。

代码位置：

- [`AccessServiceDiscovery.cpp`](../src/runtime/AccessServiceDiscovery.cpp#L250)；
- [`SmoothWeightedRoundRobinImpl.h`](../src/routing/SmoothWeightedRoundRobinImpl.h#L353)；
- [`SmoothWeightedRoundRobinImpl.h`](../src/routing/SmoothWeightedRoundRobinImpl.h#L395)。

第一阶段由本项目完成：

- service/cluster 目录改成不可变 snapshot；
- selection 不再持有目录 mutex；
- 保留当前全局 SWRR 状态和锁，先消除双层锁；
- 建立 endpoint 数 1/8/32/128、worker 数 1 到 CPU 数的 contention benchmark。

已增加默认关闭的 `fiber_access_service_selection_benchmark`。它对每个 endpoint 数执行
worker `1..max-workers` 的固定总 selection 次数，输出 CSV；测量包含 atomic directory pin、
cluster 查找和 SWRR selection，不包含请求执行与 completion/report。构建和运行方式：

```bash
cmake -S native -B native/build-benchmark -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_BUILD_TESTS=OFF \
  -DACCESS_SERVER_BUILD_BENCHMARKS=ON
cmake --build native/build-benchmark \
  --target fiber_access_service_selection_benchmark --parallel
native/build-benchmark/access-server/fiber_access_service_selection_benchmark \
  100000 "$(nproc)"
```

2026-08-17 在 20 vCPU、13th Gen Intel Core i7-13700H、WSL 虚拟化环境，以每个 case
100,000 次 selection 单次运行得到以下方向性结果；未绑定 CPU、未隔离系统负载，也未做多轮
统计，因此只能用于确认热点趋势，不能作为生产容量结论：

| endpoint 数 | 1 worker ops/s | 20 worker ops/s | 20/1 吞吐比 |
| ---: | ---: | ---: | ---: |
| 1 | 17,534,569 | 2,169,046 | 0.12 |
| 8 | 17,079,046 | 2,291,947 | 0.13 |
| 32 | 9,529,716 | 1,385,815 | 0.15 |
| 128 | 4,210,610 | 726,212 | 0.17 |

结果确认剩余的共享 selection 路径在同一 service 高并发下呈负扩展，且 endpoint 增长带来
预期的 O(N) 扫描成本。当前 benchmark 尚不能把 atomic directory pin 与全局 SWRR mutex 的
成本完全分离，而且没有计入 completion/report 的再次加锁。第二阶段应先补组件分解、多轮、
绑核和生产分布 profile，再比较 sharded/per-loop 方案的流量分布与故障状态一致性，不能仅凭
这次微基准直接改变负载均衡语义。

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

**实施状态：已解决（2026-08-17）。** `GrayMatchStore` 在 access-server runtime 中显式绑定
HTTP worker group。每次更新只编译一份带单调 generation 的不可变规则，再为每个 worker
创建独立的小型 snapshot wrapper 并同步 release-store 到 cache-line 对齐的槽位。wrapper
共享只读规则，但它们各自拥有独立的 `shared_ptr` 控制块；请求通过当前 EventLoop 的
`group_index()` O(1) 取得槽位，只修改本 worker 的引用计数和采样状态，不再访问全局 sample
sequence。配置 validator 继续使用不绑定 worker 的 canonical snapshot，不承担请求匹配职责。

采样改为每 worker 的 SplitMix64 状态，初始状态由固定 seed 和 worker index 分别混合；状态是
普通 `uint64_t`，因为只有所属 EventLoop 会访问它。错误 loop/group 会 fail closed。worker 数
在 `EventLoopGroup` 和 store 构造时固定；改变 worker 数需要重建 runtime，不支持运行中 resize。

本实现刻意没有使用异步 fan-out：`apply()` 先完成全部 wrapper 分配，再逐 worker 原子发布，
返回时所有槽位均已更新；并发请求只会在单次匹配中看到完整旧 generation 或完整新
generation。内部 generation 仅用于验证 gray 配置发布，不是 rnacos 发布状态或实例 activation
证据，也未作为控制面证据暴露。

改造前，gray 判断每次 atomic load 共享 snapshot，并通过全局 atomic sequence 产生采样值。gray
随机序列不是外部精确兼容契约，可以改为每 worker snapshot 和 PRNG，消除共享 cache
line。

代码位置：[`GrayMatchStore.cpp`](../src/runtime/GrayMatchStore.cpp#L108)。

需要验证 ratio 分布、更新 generation、固定 seed 测试和 worker 数变化，不得改变未知
entry、无效 CIDR、ratio 边界等既有兼容行为。

测试现已覆盖 10,000 个 basis-point 输入的精确 ratio 计数、固定 seed 的逐项采样结果、
1/2/4 worker 的独立序列、初始空快照、更新、clear 和 generation；原有未知 entry、无效 CIDR
及 Java 兼容 ratio 行为保持不变。本项未修改 Fiber gitlink。

### 7.4 P-04：TLS hazard 回收调度

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** `TlsCertificateStore` 现在维护原子
`retirement_pending`：普通握手清除 hazard 时只有在确实存在未回收 snapshot 才会投递
Nacos owner-loop reaper。证书 rotation 会先把旧 snapshot 标记为 pending，再立即执行一次
owner-loop reclaim；如果 worker 仍持有 hazard，后续 clear 才负责唤醒 reaper。这个先标记、
后扫描的顺序覆盖了 clear 与 publish 交错，避免漏唤醒。

TLS identity selector 使用 hazard pointer，避免握手热路径 shared ownership，这是合理的
设计。改造前每次清除 hazard 都调用 `request_reclaim()`；即使没有 retired snapshot，也会
尝试向 Nacos owner loop 投递 reaper。

代码位置：[`TlsCertificateStore.cpp`](../src/runtime/TlsCertificateStore.cpp) 和
[`AccessTlsMetrics.cpp`](../src/observability/AccessTlsMetrics.cpp)。

本次实现同时：

- 为 retired entry 记录 owner-loop retirement time；
- 暴露固定 trigger 的 rotation/reclaim 次数、retired 数量、最老对象年龄和最长保留时间；
- shutdown 同时等待 retired queue 清空和已投递 reaper 完成，保证回调先于 store 析构；
- 保留 hazard publish/clear/scan 的 `seq_cst`，未把性能修改扩大为内存模型变更。

确定性测试分别覆盖无 rotation 的高频 clear 不触发 reaper，以及 rotation 时旧 snapshot
被 hazard 持有、clear 后恰好触发回收。后续真实 TLS transport 测试又把 rotation 固定插入
ClientHello selector 已返回 v1、Fiber 尚未执行 `SSL_set_SSL_CTX` 的窗口；第一次握手继续使用 v1
成功，第二次握手选择独立的 v2 context，并验证 publish 保留、hazard clear 回收和 shutdown 回收的
完整事件序列。指标契约见
[`bounded-metrics.md`](bounded-metrics.md#tls-certificate-rotation-and-reclamation)。

Fiber 的 TLS selector 已能提供 ClientHello 和 context 选择；当前问题来自本项目的动态
证书快照实现，不要求 Fiber 修改。若未来多个应用都需要动态 TLS identity store，再单独
评估是否上游化，不能与本次修复绑定。

### 7.5 P-05：模板、URI 和 header 分配

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** 改造前明确的请求级分配包括：

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

实现采用以下边界：

- `EvaluatedTemplate` 显式区分 borrowed/owned：静态模板直接借用请求已 pin 的 compiled
  snapshot，动态模板只持有一个输出 string；expression evaluator 改为向同一 buffer
  append，不再为每段创建临时 string。编译时按 literal 加每个 expression 64 bytes 计算
  bounded reserve；默认 256 expression 上限使推测性空间最多增加 16 KiB，不会按可能很长的
  expression 源码无界 reserve；
- request target 使用一个稳定 `string_view` 和可选 owned storage。存在 `unparsed_uri` 且无
  rewrite 时直接借用 exchange 字节；只有 path/query 合成或 rewrite/escape 才写 storage，且
  所有 retry 复用同一结果；
- `ProjectRouteSnapshot` 编译时预计算 `<project>.unifiedAccess`，并把新增字节计入 snapshot
  memory budget；请求只传递稳定 view；
- compiled header 保存原始名称、ASCII lowercase 名称和完整 hash。prepared header 名称使用
  view，静态值借用 snapshot，动态值由 prepared/request vector 持有。写入 Fiber
  `HttpHeaders` 时使用 prehashed `set_view`/`add_view`，避免再次复制名称和值；
- RESPONSE 配置仍按原顺序求值、在求值后过滤 hop-by-hop header，并保留大小写重复时最后
  一个生效的语义。gzip 修改 borrowed `Vary`/`ETag` 前先 materialize；proxy header
  大小写去重、空值过滤、错误结果和 retry 只求值一次的行为不变。

生命周期依据是 `AccessRequestHandler` 在整个 coroutine 内 pin 全局 route snapshot；动态
request header value vector 在构造前按上限 reserve，并存活到所有 upstream retry/发送结束；
prepared response/proxy header 则存活到 downstream header 编码完成。没有 view 指向已移动的
SSO string 或循环内临时对象。

新增默认关闭的 `fiber_access_template_header_benchmark`。它在同一进程内覆盖静态/动态模板、
8 个 RESPONSE header 和 8 个 proxy response header，分别输出 median、31 个 batch 的 p99、
`operator new` 次数和申请字节数：

```bash
cmake -S native -B native/build -DACCESS_SERVER_BUILD_BENCHMARKS=ON
cmake --build native/build \
  --target fiber_access_template_header_benchmark --parallel
native/build/access-server/fiber_access_template_header_benchmark 10000
```

2026-08-17 在 Release+ThinLTO、Clang 22、WSL 的同一构建树中，benchmark 源先在旧实现上运行
一次，再在新实现上连续运行三次；下表的新延迟取三轮对应值的中位数，分配值三轮完全一致。
未绑核且未隔离系统负载，因此延迟仅用于确认方向，不是生产容量或端到端 HTTP p99：

| case | 旧 alloc/op | 新 alloc/op | 旧 bytes/op | 新 bytes/op | 旧 median/p99 ns | 新 median/p99 ns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| static template | 1 | 0 | 71 | 0 | 11.2 / 14.4 | 1.9 / 2.8 |
| dynamic template（4 expressions） | 8 | 1 | 950 | 268 | 124.5 / 138.0 | 24.3 / 55.2 |
| static RESPONSE headers（8） | 17 | 1 | 1,072 | 640 | 498.1 / 532.7 | 384.4 / 492.4 |
| dynamic RESPONSE headers（8） | 33 | 9 | 2,408 | 1,272 | 853.2 / 901.8 | 522.1 / 868.8 |
| static proxy headers（8） | 17 | 1 | 1,072 | 640 | 490.0 / 569.9 | 386.2 / 448.7 |
| dynamic proxy headers（8） | 33 | 9 | 2,408 | 1,272 | 888.2 / 1,372.9 | 575.8 / 611.0 |

这些行为包含 Java 模板和 header 兼容逻辑，应留在 access-server；无需修改 Fiber 通用
HTTP API。本次确认 Fiber 现有 prehashed view API 已具备所需生命周期能力，因此没有修改
Fiber、没有新增 Fiber Issue，也没有更新 gitlink。

### 7.6 P-06：初始项目批量发布

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** 改造前每个项目 commit 都复制全部 project snapshot
并重建全局 Host matcher；全局 snapshot 构建还用嵌套循环检查重复项目。数百项目逐个到达时，
会反复处理越来越大的项目集合。

代码位置：

- [`RouteConfigStore.cpp`](../src/runtime/RouteConfigStore.cpp#L99)；
- [`AccessRouteSnapshot.cpp`](../src/routing/AccessRouteSnapshot.cpp#L18)。

实现后的职责和状态边界如下：

- `RouteConfigStore` 的 owner-loop registry 改为按 project 排序的透明比较 `map`，一条 record 同时
  持有最后成功版本和可选 loaded snapshot。project 唯一性由 registry 保证，版本查询从线性扫描
  改为 O(log N)；unload 只清除 snapshot 并保留最后成功非空版本，project-list remove 才删除
  record；
- 新增 `commit_batch(vector<ReadyProjectUpdate>)`。它仍只接受完成 compile、selector bind 和
  service-ready 的 move-only typestate token；按 project 名确定性排序，组装完整候选后只调用一次
  `AccessRouteSnapshot::build()`，成功后只做一次 atomic shared snapshot publish；
- 批量提交先用与 `HostMatcher` 完全相同的 Java byte-fold identity 做线性 Host ownership 校验。
  一个候选冲突时恢复该 project 的旧 Host 所有权和旧 snapshot，其他合法候选仍进入最终批次；
  因此错误输入也不会退化回 N 次全局 rebuild，且保留失败候选不替换旧 snapshot 的语义；
- `AccessConfigWatcher` 只在启动时 store 为空的首轮同步开启 initial batch。项目进入显式
  `ReadyToPublish` 状态并暂存在对应 `ProjectEntry`；当前 project-list 的每个项目取得首个终态后
  才统一 commit。`AccessServerRuntime` 本来就要等待同一 readiness 才启动 listener，因此不会增加
  对外可用延迟，也不会暴露半套初始路由；
- 同步期间同项目新 generation、project-list remove、subscription close 和 shutdown 都通过
  `ProjectEntry::advance()` 销毁 staged token。首轮结束后恢复逐候选热更新；same-version、invalid
  candidate、unload/remove 和失败保旧行为不变；
- `AccessRouteSnapshot::build()` 的防御性 project 去重由嵌套循环改为局部 hash set，即使绕过
  store 直接调用仍会校验唯一性，但复杂度降为期望 O(N)；
- readiness/Prometheus 新增 bounded `ready_to_publish` 数量；配置阶段指标只使用固定的
  `project_compile`、`service_ready`、`global_build`、`publish` 四个 label，分别输出 observation、
  累计纳秒和最大纳秒，不包含 project、Host 或 Data ID。

新增默认关闭的 `fiber_access_route_publication_benchmark`。每轮在计时外完成相同 project
snapshot 的 compile/prepare，然后比较逐项目 commit 控制路径与一次 batch commit；逐项路径等价于
旧启动发布方式，但已经享受新 registry 和 O(N) 去重，因此结果是保守对照：

```bash
cmake -S native -B native/build -DACCESS_SERVER_BUILD_BENCHMARKS=ON
cmake --build native/build \
  --target fiber_access_route_publication_benchmark --parallel
native/build/access-server/fiber_access_route_publication_benchmark
```

2026-08-17 在 Release+ThinLTO、Clang 22、WSL 的同一构建树中连续运行三轮；下表取三轮各自
7 个 sample 中位数的再次中位数。未绑核且未隔离系统负载，数值只用于扩展趋势，不是生产容量：

| projects | 逐项 commit median | batch median | 约加速 | snapshot publications |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 10.3 us | 4.1 us | 2.5x | 10 → 1 |
| 100 | 1,280.9 us | 66.3 us | 19.3x | 100 → 1 |
| 500 | 74,696.4 us | 660.5 us | 113.1x | 500 → 1 |

本项是 access-server 的配置编排、Java Host 兼容和发布策略，完全由本项目实现；没有修改 Fiber、
没有新增 Fiber Issue，也没有更新 gitlink。

### 7.7 P-07：Host matcher 高 fan-out

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** 改造前 `HostMatcher::find_child()` 对当前节点的
children 全量线性扫描。通常域名层数和同级 label 数量都很小，但大量租户共享同一域名后缀时，
每个请求都要扫描高 fan-out 节点。

代码位置：[`HostMatcher.cpp`](../src/routing/HostMatcher.cpp#L165)。

实现采用紧凑的混合索引，不给每个 trie node 增加 hash table：

- `HostMatcher::build()` 仍以原顺序完成重复检测、exact/wildcard handler 组装；构建完成后，
  只对超过 16 个 child 的节点按 Java-fold 后的 label 排序。child 引用的是稳定 node index，
  因此边重排不会改变 handler 或节点生命周期；
- `find_child()` 在 16 个及以下 child 时保留 cache-friendly 线性扫描，超过 16 个时使用
  `lower_bound`。比较器逐字节执行现有 `byte | 0x20` fold，不生成 lowercase request string，
  请求路径仍然零分配；
- 排序和查找共用同一 unsigned-byte fold 顺序，保留 Java 的非标准 ASCII 标点等价关系；Host
  语法校验、尾点/端口处理、wildcard 匹配，以及 exact child 阻止祖先 wildcard fallback 的行为
  均未改变；
- `AccessRouteSnapshot` 继续只负责拥有和调用全局 matcher，不感知索引实现；没有增加请求 worker
  状态、共享锁或高基数指标。

新增默认关闭的 `fiber_access_host_matcher_benchmark`。它在计时外用逆序 pattern 构建共享后缀
trie，执行固定的 75% hit / 25% miss 查询（hit 使用大写以覆盖 fold），每个 sample 65,536 次
lookup：

```bash
cmake -S native -B native/build -DACCESS_SERVER_BUILD_BENCHMARKS=ON
cmake --build native/build --target fiber_access_host_matcher_benchmark --parallel
native/build/access-server/fiber_access_host_matcher_benchmark
```

2026-08-17 在 Release+ThinLTO、Clang 22、WSL 的同一构建树中，旧实现和最终实现各连续运行
三轮；每轮结果是 7 个 sample 的中位数，下表再取三轮中位数。未绑核且未隔离系统负载，结果
只说明 matcher 扩展趋势，不代表完整请求延迟：

| sibling fan-out | 旧 linear ns/lookup | 新 hybrid ns/lookup | 约加速 |
| ---: | ---: | ---: | ---: |
| 1 | 53.23 | 52.84 | 1.0x |
| 4 | 51.43 | 51.49 | 1.0x |
| 8 | 56.71 | 53.76 | 1.1x |
| 16 | 63.35 | 59.96 | 1.1x |
| 64 | 107.49 | 74.43 | 1.4x |
| 256 | 211.66 | 91.96 | 2.3x |
| 1024 | 767.96 | 132.82 | 5.8x |

额外 crossover 对照中，16 child 的线性路径优于二分，32 child 的二分约 68 ns、线性约
77 ns，因此最终阈值取 16。该策略属于 access-server 的 Java Host 兼容与路由数据结构，完全
由本项目实现；没有修改 Fiber、没有新增 Fiber Issue，也没有更新 gitlink。

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

**实施状态：已解决（2026-08-17）。** `AccessServerRuntime` 已从进程级“大对象”收敛为
112-byte composition-root façade，公开的 `create()`、`start()`、`shutdown()`、`state()`、
`fd()` 和 `metrics_fd()` 契约保持不变。内部职责按以下边界拆分：

- `AccessRuntimeFactory` 只校验进程级 upstream TLS policy，构造 Nacos/CAT 客户端，并把配置
  投影为 control/data plane options；所有创建失败继续映射到原有 typed runtime error；
- `AccessControlPlaneSupervisor` 独占 Nacos/CAT、compiler、service discovery、route/gray/TLS
  store、三个 watcher、activation evidence 和 runtime metrics，所有 Nacos/CAT 操作仍投递到各自
  owner loop；
- `AccessDataPlaneService` 独占 script runtime、业务/metrics listener 和 worker resources，借用
  control plane 已发布的 store、metrics、evidence 与 CAT client；listener 只会在配置与 TLS
  readiness 成功后创建；
- `AccessRuntimeCoordinator` 只持有两个平凡可复制的非 owning function-table adapter，以显式
  `Created -> Starting -> Running -> Stopping -> Stopped` 状态机编排生命周期；该间接调用只发生在
  进程启动和关闭，不进入请求热路径；
- `AccessServerRuntimeError` 独立承载稳定 error code、stage name 和 error builder，避免 factory、
  supervisor 与 data plane 反向依赖 façade 实现。

启动顺序固定为 control plane readiness 后启动 data plane。control plane 启动失败时不会触碰
data plane；data plane 初始化或任一 bind 失败时先关闭已尝试的数据面，再按 CAT、watcher/TLS
store/route/discovery、NamingService、ConfigService、Nacos client 的既有顺序回滚。正常关闭同样先
data plane 后 control plane；未启动实例仍释放其已拥有的 control-plane 客户端。重复或同一 owner
loop 上并发的 shutdown 由一个有界 lifecycle watch 合并，runtime components 本身没有共享所有权。
TLS bootstrap 临时文件仍在业务 listener 完成证书加载后立即关闭。

新增的 concrete fake 测试覆盖成功启动顺序、control-plane 失败、data-plane 失败逆序回滚、未启动
关闭，以及两个并发 shutdown 与后续重复 shutdown；另有结构约束锁定 façade 不超过 128 bytes、
无多态，以及两个 adapter 的 trivially-copyable 性质。原有 loopback/runtime 测试继续覆盖真实
worker 初始化、业务/metrics bind、TLS、Nacos/CAT owner-loop 和完整 shutdown。

拆分刻意没有引入虚基类、`std::function`、锁、runtime component 的共享所有权或请求级分配。
对象布局合计从 7,544 bytes 增至 7,712 bytes（+168，约 +2.2%），并增加两个仅在启动期发生的
component allocation；相同 Release ThinLTO 构建中最终二进制 text 从 6,914,601 增至
6,931,001 bytes（+16,400，约 +0.237%），文件大小增加 26,344 bytes（约 +0.149%）。这是为隔离
translation unit 和可替换 lifecycle seam 支付的冷路径成本，请求处理对象、coroutine 层数及热路径
保持不变。

### 8.2 C-01：`AccessServer`

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** `AccessServer` 现在只保留业务 `HttpServer`、
`AccessWorkerResources`、`AccessMetricsEndpoint` 和四个 façade 生命周期入口。
`AccessWorkerResources` 独占 client metadata/access log、DNS、pool、proxy/handler、worker
metrics 和 CAT worker detach；`AccessMetricsEndpoint` 独占 status listener、Prometheus 渲染
及 activation 分流。关闭仍严格按 metrics listener、业务 listener、metrics collection、CAT
worker context、pool、DNS 排序，之后才由 runtime 停 CAT client。提取没有增加虚调用、
`std::function`、共享所有权、请求期分配或跨 loop post，原有 loopback façade 测试覆盖业务、
metrics、activation、CAT 和关闭行为。

实现保留其 data-plane façade 角色，将 DNS、pool、CAT worker attachment 抽成
`AccessWorkerResources`，metrics listener 抽成 `AccessMetricsEndpoint`；`AccessServer`
只负责 initialize、bind、serve 和全异步 shutdown。

### 8.3 C-01：配置 watcher

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** 新增 owner-loop-only 的 concrete
`SubscriptionLifecycle`，按值组合进 project-list、每个 project route、gray 和 TLS watcher。
它统一拥有单条 Nacos `Subscription<ConfigData>` handle，并集中维护：

- `Created/Subscribing/Subscribed/Retrying/Failed/Stopped` 状态；
- 单调 generation 和 revision watch，供 retry、off-loop compile 及 service-ready await 丢弃旧结果；
- 当前订阅是否收到首值；
- retry attempt、`next_retry_at`、暂态错误分类及封顶指数退避；
- 仅含 typed error code/`IoErr` 的紧凑订阅失败，不重复持有可能敏感的错误消息；
- 同步 cached replay：`ConfigService::subscribe()` 返回 handle 前发生的 Success/Closed callback 不会
  被随后安装 handle 覆盖；
- 幂等 stop、owner-loop handle close 和析构时无活动订阅的不变量。

资源职责没有进入公共 lifecycle：`AccessConfigWatcher` 继续独占两级订阅图、project reconcile、
compile queue、initial batch、service readiness、route publish、typed readiness、metrics 和 activation
evidence；Gray 继续负责兼容 decode 与 per-worker snapshot；TLS 继续负责 coalesced off-loop compile、
证书 prepare/commit、bootstrap readiness 和 processing watch。三个 watcher 的公开 state、失败类型、
计数器和 API 不变，没有形成继承树。

行为也保持原契约：Gray/TLS 意外关闭仍是 terminal `Failed`，project-list 关闭仍发布
`Unavailable`，仅 project route 的暂态首次订阅错误按原配置 retry，永久错误仍为 `Failed`；项目删除、
新值和 shutdown 都推进 generation，从而取消旧 retry/compile/service-ready 结果。每个 project 原有的
revision watch 被 lifecycle 直接替换，没有叠加；只有 project-list、gray 和 TLS 三个 control-plane
根订阅各新增一个 watch shared state。

新增单元测试覆盖 104-byte 非多态 lifecycle、同步 cached replay、Closed generation 失效、三档退避
封顶、永久失败、启动失败回滚和重复 stop。既有 access/gray/TLS watcher 测试继续覆盖 typed
readiness、retry 取消、旧快照保留、initial batch 及 off-loop compile。实现没有新增虚调用、
`std::function`、锁或请求路径逻辑；相同 Release ThinLTO build 中 access-server text 从
6,931,001 增至 6,933,273 bytes（+2,272，约 +0.033%），文件大小增加 3,528 bytes（约
+0.020%）。

### 8.4 C-01：`RouteConfigStore` 和 `ProjectRouteSnapshot`

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** 现在按以下边界组合 concrete component：

- `ProjectConfigCompiler` 只负责 `ProjectConfig -> ProjectRouteSnapshot`：资源预算、Host/Path、
  method、CIDR/address、脚本/模板、静态 gzip、常量包和 matcher；它借用 compiler adapter 与固定
  limits，不持有 Nacos、版本 registry 或发布状态。既有 `compile_project_config()` 作为 validator、
  benchmark 和测试的兼容入口保留，但实际 runtime 编译直接使用该类；
- `ProjectSnapshotRegistry` 是 owner-loop-only 的有序 registry，一条 record 只含最后成功非空版本
  和可选 loaded snapshot。`replace/unload/remove` 分别表达替换、保留版本的卸载、删除版本；
- `RouteSnapshotPublisher` 是唯一跨线程边界，独占 atomic `shared_ptr` 的 acquire pin 与 release
  publish；`RouteConfigStore::pin()` 是 inline delegate，ThinLTO 最终二进制中没有额外 pin symbol；
- `RouteConfigStore` 只编排 Prepared/Ready typestate、单项及批量事务、跨项目 Host 校验、完整候选
  build 和阶段耗时。registry 只在完整 `AccessRouteSnapshot` 构建成功后变更，然后执行一次 release
  publish；
- `ProjectRouteSnapshot.cpp` 从 1,194 行降为 57 行，只实现 immutable snapshot 的 header 元数据构造、
  Host 查询、selector readiness 查询/等待；1,176 行有界构建逻辑移入独立 compiler translation unit。

`ProxyAddressSelectorFactory` 的回调现在返回显式 `expected<shared_ptr, Error>`。
`AccessServiceSelectorFactory` 不再保存 `acquire_error_`，也不再要求调用方配对
`begin_compile()/take_error()`；每次 NamingService acquire 的错误直接属于该次调用。selector binding
仍遍历全部 service route、局部保留第一个错误后返回，因此继续为两条失败 route 记录两次 bounded
metric，同时不存在跨编译粘连、重入覆盖或忘记清空错误的状态。

兼容语义保持不变：同版本忽略；空 Host unload 但保留最后成功版本；project-list remove 才删除
version；批量重复 project 整批拒绝，Host 冲突只拒绝对应 project；失败候选不替换旧 snapshot；旧请求
pin 在热更新后继续有效。请求路径仍只有一次 atomic shared ownership acquire，没有新增锁、分配、
虚调用或 `std::function`。

组件测试锁定三个类均为非多态，64-bit 布局分别为 compiler 32 bytes、registry 48 bytes、publisher
16 bytes；另覆盖 registry 排序/unload/remove、旧 pin 生命周期，以及两 route acquire 失败后下一次
prepare 可恢复且无 sticky error。相同 Release ThinLTO build 中 access-server text 从 6,933,273 增至
6,934,609 bytes（+1,336，约 +0.019%），data/bss 不变，文件大小增加 1,520 bytes（约 +0.009%）。

同一未绑核 WSL 会话的 publication benchmark 在改造前单轮为 10/100/500 projects 的 sequential
`15.1/1580.0/82668.6 us`、batch `5.7/72.7/693.7 us`；改造后三轮中位数为 sequential
`11.6/1345.6/72420.2 us`、batch `4.7/67.9/647.6 us`。该数据只用于排除明显回退，不宣称吞吐提升；
批量仍保持一次原子发布和随项目数增长的原有加速方向。

### 8.5 C-01：`AccessRequestHandler`

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** Handler 保留直线式 pipeline，只抽出了：

- `ClientMetadataResolver`：peer/trusted proxy/scheme/source address；
- `RoutePolicyEvaluator`：entry、HTTPS、body、CIDR 纯策略。

`RoutePolicyEvaluator` 是无虚函数、无锁、无分配的小型 concrete object，返回 host/route
typed decision；Handler 继续负责 snapshot pin、Host/Path 匹配、request context、异常映射、
redirect I/O 和 executor 选择。拆分保留了 Java 兼容的判断顺序：entry 先于 HTTPS，已知
`Content-Length` 先于 CIDR，allow 先于 deny，legacy 缺失源地址跳过 CIDR，route 负数 body
limit 表示 unlimited。纯单元测试同时覆盖 IPv4/IPv6、全部 redirect status 和 script
unknown-length 限制，原 Handler 集成测试继续验证相同 wire error 和执行顺序。

### 8.6 C-01：`ProxyExecutor`

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** `ProxyExecutor::execute_impl` 现在只编排 context/gray、
选址、失败 token、最多四次连接尝试和最终 attempt；同步 request target/header/template/body
framing 由独立 `ProxyRequestPlan` 一次性准备，连接重试只重绑未被配置覆盖的 Host。连接成功
后的 header 注入、request body、最终 response header、response rewrite/body pipe 和 WebSocket
tunnel 由 concrete `UpstreamAttempt` 负责。

没有再包装新的 `HttpResponseBridge`/`WebSocketBridge` coroutine：普通 body 继续直接使用 Fiber
`pipe_http_body`，WebSocket 继续直接使用 `relay_websocket_tunnel`。`UpstreamAttempt` 刻意留在
`ProxyExecutor.cpp` 同一 translation unit，使 Release 编译器能够 elide 嵌套 Task allocation；
同时 `execute_adapter` 改为直接返回 Task，不再创建转发 coroutine。

Clang 22、`-O3`、关闭 LTO 的可重复对象级检查中，应用层 active coroutine frame 从
`648 + 440 + 2568 = 3656` bytes 降到 `440 + 2272 = 2712` bytes（约 -25.8%），frame 数从
三个降到两个；拆分前单对象 text 为 46522 bytes，拆分后 Executor 与 RequestPlan 合计
42547 bytes（约 -8.5%）。检查未发现 `UpstreamAttempt` 的额外 `operator new`。实现没有增加
`std::function`、虚调用、共享所有权、锁或请求期容器分配；聚焦测试继续覆盖模板只求值一次、
Host retry rebind、body limit、selection report、response limit、取消和 WebSocket 双向 relay。

### 8.7 C-01：`AccessRequestTelemetry`

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** `AccessRequestTelemetry` 保留为调用方唯一持有的
request-lifetime façade，公共方法和调用点不变，内部改为按值组合 `ScriptExecutionContext`、
`ClientMetadata`、`TracePropagation` 和 `RequestObservability`。`AccessProviderTransaction` 也已
从 façade 中拆出为独立的 move-only CAT transaction guard。

职责边界如下：

- `ScriptExecutionContext` 独占 GC heap、`ScriptExchangeCtx` 和待发送 response headers，负责
  request pool copy 及动态常量 bind/clear；
- `TracePropagation` 独占 W3C `traceparent`/`tracestate`、CAT propagation context 和已绑定常量包，
  负责 context 更新以及 downstream/upstream trace header；CAT transaction 只作为显式参数传入；
- `RequestObservability` 独占 bounded metrics、CAT root/events、access-log 字段、采样及 terminal
  accounting；它不持有 execution/metadata/trace 回指针；
- façade 只编排跨职责操作，例如 project cluster 同时进入 CAT 字段和 trace context。

构造顺序固定为 execution、client metadata、trace storage、observability。observability 先记录
owner-loop start time 和 `request_started`，再初始化 trace，保持原先 duration 覆盖范围；façade
析构体在成员逆序析构前显式完成 metrics、CAT 和 access log，因此 finish 时所有借用对象仍然存活。
简单 façade 委托保持 header-inline，Release 调用点不会多经过一层 trampoline。

新增 `AccessRequestTelemetryTest` 锁定 64-bit façade 的 864-byte 基线和非多态组件，分别覆盖 CAT
禁用及 CAT wrong-loop 创建失败、W3C 常量 bind、context update/remove、tracestate upstream 注入、
无 CAT 时的 header 行为和 request-pool storage。原 Handler/Proxy 集成测试继续覆盖真实 CAT root、
`Access.Provider`、`RemoteCall`、`CALL_ERROR`、`FiberException`、`ResponseError`、指标、代理 header
和脚本结果。

Clang 22 `-O3` 布局复测中，四个成员分别为 432、112、184、136 bytes，总计仍为 864 bytes；没有
新增堆包装、虚表、`std::function`、共享所有权、锁或 coroutine。相同 Release ThinLTO build tree
的最终 access-server text 从 6,913,705 增至 6,914,941 bytes（+1,236，约 +0.018%），文件大小
增加 1,744 bytes（约 +0.010%）；拆分后的五个无 LTO 实现对象 text 合计 19,309 bytes，低于拆分前
单对象的 19,569 bytes。

### 8.8 C-01：`AccessConfigCodec`

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** 公共 `AccessConfigCodec.h`、三个解析入口和返回类型保持
不变，原先 1,136 行的单实现文件已按 wire contract 职责拆分：

- `AccessConfigJson` 只负责将 Fiber tokenizer 物化为 pool-backed、trivially-copyable JSON 树，
  保留浮点数原始 token spelling 和 parser byte offset；
- `AccessConfigErrorBuilder` 统一构造 error code、脱敏消息、子字段路径和数组下标；其非模板
  `DecodeFailure` 避免不同 codec translation unit 重复实例化整套 error builder；
- `AccessConfigCoercion` 集中实现 Java/Jackson scalar coercion、duration/data-size、gzip、enum，
  以及 project 与 gray 共用的 string map/set；
- `ProjectConfigCodec` 独占 project、host、route、body 模型解码，`GrayMatchConfigCodec` 独占 gray
  rule 解码；`AccessConfigCodec.cpp` 只保留分号分隔的 project-list wire codec。

拆分继续使用一次 `BufPool` 和一棵借用输入/pool storage 的 JSON 树，所有模型转换都在 pool
析构前同步完成；没有改成反射式 codec 或未经 profile 的流式 parser。兼容不变量包括：空内容与
JSON `null` 的资源级差异、unknown field 忽略、duplicate field 按输入顺序处理且 last-wins、Java
int/Boolean/String/enum coercion、duration int overflow、data-size long bit behavior，以及原始 JSON
错误 offset 和嵌套 field path。新增测试锁定 malformed JSON offset 11、
`routes[0].proxy_headers.X`，以及 gray duplicate/unknown/null/scalar 行为；既有 Java fixture、limits、
watcher 和 validator 测试继续覆盖完整入口。

配置 decode/compile 已在此前的 C-02 中移出 Nacos owner loop，本次没有触碰请求热路径，也没有
新增反射、虚调用、`std::function`、共享所有权、锁或 coroutine。Clang 22 `-O3` 无 LTO 对象
text 从单对象 85,717 bytes 变为六个对象合计 89,675 bytes（+3,958，约 +4.6%，主要是独立 TU
边界）；实际 Release ThinLTO access-server text 从 6,914,941 降至 6,914,601 bytes（-340，约
-0.005%），文件大小增加 400 bytes（约 +0.002%）。

## 9. 可观测性和实例激活证据

### 9.1 O-01：实例级激活证据

**归属：本项目，涉及 native、server 和 web。**

**实施状态：已解决（2026-08-17）。** rnacos write/readback 仍只证明发布结果；实例激活现在
由 access-server 类型化回执、独立 collector、带 TTL 的实例观察和 Release 精确资源比对证明。

已提供经过认证、有界、分页的 contract version 1 实例状态协议，包含：

- instance ID、build revision、启动时间；
- resource kind、Data ID、group、observed MD5/version；
- decode、compile、service-ready、publish result；
- 当前 global snapshot generation/fingerprint；
- 当前 process-wide immutable snapshot generation、fingerprint 和 request-pin 发布模式；
- TLS snapshot version 和 certificate count；
- discovery ready 和有界聚合数量；
- 脱敏的最后失败及发生时间。

高基数明细没有进入 Prometheus label。证据路径与 metrics 共用状态 listener，但独立执行
Bearer 认证、分页和 `no-store`；server 的独立进程负责认证采集、租约、持久化、TTL 和环境
隔离，token 不入库也不交给 API 进程。web 继续区分 draft、published 和 instance activation，
并按需分页展示实例证据。缺少目标或证据过期仍严格显示 `unknown`。协议与运维说明见
[`../../../docs/activation-evidence.md`](../../../docs/activation-evidence.md)。

### 9.2 O-02：有界运行指标

**归属：双方。聚合与产品语义属于本项目；真实 Nacos 连接证据需要 Fiber。**

**实施状态：部分解决（2026-08-17）。** 第一阶段新增 Nacos owner loop 单写、metrics worker
无锁读取的 `AccessConfigMetrics`。Project List/route 的 success、ignored、failure stage 使用
编译期固定 `resource/result/reason` 表；readiness 使用固定 one-hot state 和六个 Project 聚合；
每次实际发布同时更新 process-local snapshot generation、age、Project/Host/route/program 数量、
估算内存和静态响应字节。配置值、Data ID、MD5、Project、Host、route 和 service 均不进入 label。
第二阶段增加 `AccessRuntimeMetrics` 聚合边界和 `AccessDiscoveryMetrics`：报告 client/config/naming
的应用生命周期、ready service、可选择 endpoint、logical cluster、selector lease，以及固定
分类的 update/retire/acquire 结果。聚合来自现有选择模型，不扫描请求路径；跨线程 selector
析构只更新原子 lease gauge。具体 schema、并发模型和非 activation 语义见
[`bounded-metrics.md`](bounded-metrics.md)。

第三阶段随 P-04 增加 `AccessTlsMetrics`：报告证书 rotation、按固定 trigger 分类的 reclaim
scan/reclaimed snapshot、当前 retired snapshot 和最长保留时间。普通 TLS 握手不更新指标、
不投递 owner loop，只有实际 rotation 后的 hazard clear 才可能触发回收。

第四阶段增加 worker-sharded proxy/DNS/pool/WebSocket 指标：execution 和 selected-upstream
attempt 具有取消安全终态及 inflight；连接 acquisition 通过固定 POD 回传 pool hit/miss/shutdown、
DNS success/empty/failure/unavailable 和 connect success/failure/TLS/create failure；proxy failure
phase 以及 WebSocket handshake/session outcome 仅使用编译期枚举。记录路径不注册 label、不构造
字符串、不加锁也不跨 loop 投递。Fiber WebSocket relay 当前不返回 typed close reason，因此仅区分
relay 返回的 `closed` 与协程取消的 `aborted`，不猜测 timeout 或 peer 原因。详见
[`bounded-metrics.md`](bounded-metrics.md#proxy-transport-and-websocket-metrics)。

第五阶段接入 Fiber 已提供的 LoggerManager queue/appender 和 CatClient 原子 stats。新建的
`AccessProcessMetrics` 只在 scrape 时构造无标识快照：日志覆盖 queue backlog/peak/accepting、
queue failure、主 appender 写入/丢弃/错误/rotation；CAT 覆盖 disabled/created/running/stopping/
stopped、普通与 system backlog、submitted/sent 以及 15 个固定 loss reason。主 appender ID 由
`LogConfigBuilder` 的实际返回值从 main 显式传入，不假定为 0；CAT 指针由 runtime 在构造时绑定。
抓取只读取 Fiber atomics，不修改 request/log/CAT 提交热路径，也不暴露 appender 名、路径、CAT
app key、host、collector/router 地址或 trace。内部阶段 loss 可能重叠，不能求和冒充唯一消息数。
详见 [`bounded-metrics.md`](bounded-metrics.md#asynchronous-logging-and-cat-pipeline-metrics)。

`start()` 成功只报告 `running`，绝不冒充 transport connected。真实连接、认证和 reconnect
状态需要 Fiber 公共 typed snapshot/watch，已提交
[fiber-gateway-cpp #27](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/27)。O-02 当前唯一
未覆盖项是该上游连接证据：

建议增加：

- Fiber #27 落地后接入 Nacos config/naming transport/reconnect state；

project、route、cluster、host、Data ID、service name、header 和用户数据不得成为无界 label。
Fiber 的 pool lease、LoggerManager queue stats 等已有接口应直接消费；只有确实缺少通用、
有界统计时才提交上游 API。

## 10. 构建边界

### 10.1 C-02：拆分 `access_server_core`

**归属：本项目。**

**实施状态：已解决（2026-08-17）。** 原 `access_server_core` 已拆为五个显式 static
component，源码仍逐项注册且每个源文件只属于一个 target：

```text
access-gateway-validator -> validation -> config -> fiber_lib + zlib
access-server -> runtime -> execution -> observability -> config
                       \-> Fiber Nacos/CAT/Prometheus
```

具体边界为：

- `access_server_config` 拥有 codec/limits、不可变 routing model、Host/CIDR、route/script、
  gray 和静态 gzip 编译；
- `access_server_observability` 拥有固定 schema metrics、activation evidence、日志策略和
  trace state，不依赖 execution/runtime；
- `access_server_execution` 拥有 handler、response/proxy pipeline，以及与请求执行类型互相
  约束的 telemetry/trace façade；
- `access_server_runtime` 才拥有 Nacos/discovery、watcher、DNS、listener、worker 和发布生命
  周期，并且只有该 target 注入 build revision；
- `access_server_validation` 只编排版本化 validator protocol，私有依赖 config。

为消除原有的反向依赖，本次同时完成三个窄接口调整：

1. 从 `AccessScriptRuntime` 提取有状态的 `AccessScriptCompiler`。compiler loop 和 validator
   直接持有 compiler；请求 runtime 只剩无状态执行 adapter，不再初始化第二套编译 library；
2. 从 `GrayMatchStore` 提取纯 `GrayMatchCompiler`。validator 的严格字段校验与 runtime 的
   Java 兼容发布继续不同，但两者共享同一个 bounded CIDR/ratio compiled model；
3. handler 不再包含 `RouteConfigStore`/Nacos 头，而是消费两个指针大小、trivially-copyable
   的 `AccessRouteSnapshotProvider`。每请求仍只执行一次 acquire pin，旧快照生命周期不变。

新增默认开启的 `ACCESS_SERVER_BUILD_RUNTIME`。在全新 build tree 中将其关闭时，Fiber 不生成
Nacos、CAT、Prometheus target，CMake 仍成功构建 validator。最终 validator link 从改造前的
`core + Nacos + protobuf + CAT + Prometheus` 收敛为
`validation + config + fiber_lib + TLS/crypto + zlib`，证明不是仅依赖 LTO dead stripping：

```bash
cmake -S native -B native/build-validator-only -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_BUILD_TESTS=OFF \
  -DACCESS_SERVER_BUILD_RUNTIME=OFF
cmake --build native/build-validator-only \
  --target fiber_app_access_gateway_validator --parallel
```

同一 Release+ThinLTO、Clang 22 构建中，validator 的 text/data 从
`3,246,157/69,448` bytes 降到 `3,239,009/69,192`（text `-7,148`，约 `-0.22%`），bss
保持 `9,849`。主服务 text 从 `6,934,609` 到 `6,935,553`（`+944`，约 `+0.014%`），
data/bss 不变，文件大小从 `17,681,744` 到 `17,682,848`（`+1,104`，约 `+0.006%`）；该微小
边界成本没有改变请求算法、分配或锁形状。

完整 target 图、PUBLIC/PRIVATE 规则和独立构建门禁见
[`build-boundaries.md`](build-boundaries.md)。本项没有修改 Fiber、没有新增 Fiber Issue，也没有
更新 gitlink。

## 11. 测试与性能基线

### 11.1 T-01：正确性和并发测试

**归属：本项目；涉及 Fiber 改动时同时运行上游测试。**

**实施状态：部分解决（2026-08-17）。** `AccessRuntimeCoordinatorTest` 已覆盖 control/data
启动顺序、两层同步失败、data-before-control 逆序回滚、启动前 shutdown，以及运行态并发和
重复 shutdown。本轮又按 `main.cpp` 的实际
`when_any(runtime.start().select(), SIGINT, SIGTERM)` 取消形状，增加两个确定性回归：

- signal 分支在 control-plane start 挂起时获胜，loser task 被销毁，shutdown 只释放 control；
- signal 分支在 data-plane start 挂起时获胜，loser 不再恢复，shutdown 严格执行
  data -> control；
- 两种路径都验证 `Starting -> Stopped`、后续重复 shutdown 无副作用，并且没有依赖真实
  process-global signal 或墙钟时间。

DNS 生命周期子项也已补齐：`AccessDnsService` 通过两个指针大小的
`AccessDnsResolverFactory` 注入 cold-path resolver stack 创建，默认 factory 使用 `nothrow new`
并保留原有 resolver 参数。每个已尝试的 worker entry 无论 factory 成功或失败都先转入 service
所有权，失败统一在各 owner loop 释放 resolver/local，最后关闭共享 cache。确定性测试在第二个
worker 完成资源创建后注入失败，验证首次 init fail closed、同一 service 可重新 init 成功、异步
shutdown 和重复 shutdown 完成；既有测试继续证明 shutdown 不阻塞 control loop，且 worker group
必须在释放完成前保持可服务。

Project route 订阅竞态子项也已补齐：测试先保留一个已发布的 v1 快照，将 v2 编译确定性地挂起在
尚未启动的 compiler EventLoop，然后注入非预期 `Closed`。测试验证关闭会推进 generation、取消
编译、进入不可重试的 `Failed`、保持 readiness 为 synchronizing，并继续保留 v1；随后通过
project-list 显式删除/重加和同步缓存回放恢复到新 entry。compiler 启动后旧 v2 completion 与新
v3 job 依次返回，最终只有 v3 可以发布，从 watcher 集成层覆盖 subscription closed、stale
generation、旧快照保留和显式 reconcile 恢复的组合边界。

初始配置到达顺序子项现在使用可确定性门控的 service selector 覆盖三种顺序：service-ready 和
route 均早于 project-list 的同步缓存回放、project-list 早于 route，以及 route 早于
service-ready。一个已准备好的 Project 不会提前发布部分 snapshot；另一个 Project 的 v1 真正
挂起在 service readiness 后再被 v2 revision 取代。放行 readiness 后只发生一次 batch publish，
其中包含早到 Project 与当前 v2 generation，旧 v1 从不进入可见快照。

Control-plane 资源级回滚子项也已完成。CAT 与 Nacos concrete client 通过三个指针大小的非多态
cold-path lifecycle adapter 获得确定性故障注入，生产默认仍直接调用原 Fiber client；ConfigService
和 NamingService 保持既有虚接口。supervisor 区分外部服务 `start_attempted` 与 watcher
`started`，失败时只清理可能获得资源的阶段，并严格按 access watcher、TLS watcher、gray watcher、
TLS store/route/discovery、NamingService、ConfigService、Nacos client、CAT 的顺序回滚。参数化
多 EventLoop 测试覆盖四个 client/service 启动、三个 watcher subscribe、initial readiness 和正常
启动；subscription handle 的 close 事件用于验证真实资源次序，重复 shutdown 无副作用。

Data-plane 资源级回滚子项也已完成。现有 `AccessDnsResolverFactory` 沿
`AccessDataPlaneOptions -> AccessServerOptions -> AccessWorkerResourcesOptions` 透传，生产默认仍使用
system resolver，测试则可在完整 `AccessDataPlaneService` 边界确定性注入 worker initialize 失败，
且不在请求热路径增加虚调用或共享所有权。`start()` 的 allocation、worker initialize、业务 bind 和
metrics bind 失败统一先关闭 TLS bootstrap，再通过 `AccessServer::shutdown_and_wait()` 严格按
metrics listener、业务 listener、worker resources 逆序等待清理完成，错误返回后外层协调器的重复
shutdown 无副作用。集成测试还用真实 loopback 端口冲突证明两种 bind 失败均不遗留 fd；metrics bind
失败前已成功获得的业务端口在 `start()` 返回时即可重新绑定。

TLS rotation/handshake 子项也已补齐。测试以非阻塞 `socketpair` 并发驱动真实 client/server TLS
transport，并用测试 selector 把预编译 v2 的 commit 固定插入 v1 identity 已选中、Fiber
`SSL_set_SSL_CTX` 尚未消费返回值的窗口。第一次握手必须在 rotation 中继续成功，第二次握手必须
选择不同的 v2 context；固定容量 observer 同时验证 publish 扫描保留 v1、deferred hazard clear
恰好回收 v1、retired 数归零，以及 shutdown 回收当前 v2。测试不使用公网、端口竞争或 sleep。

上述测试已经覆盖 concrete control-plane 与 data-plane 启动阶段，但还不等价于请求并发及外部互操作
均已完成故障注入。仍应优先增加：

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

**实施状态：状态模型已解决（2026-08-17）；实际生产/切流 gate 仍为 `NOT_MET`。**

[`script-corpus-differential.md`](script-corpus-differential.md) 现在是唯一分层证据记录，明确
区分：测试环境有限语法快照、外部配置 compile-only、Java golden、仓库内 C++ 请求样例、
完整生产 corpus、阶段 8 全量差分和最终切流。原有 `352/352` 被严格限定为 2026-07-31
测试环境配置的 decode + compiled snapshot，不再外推为 352 个项目的请求级差分；文档中
引用但代码中不存在的 `MatchesProductionRewriteCorpus` 也已删除；仓库内聚合场景改名为
`MatchesRecordedConditionAndTemplateSyntaxSnapshot`，rewrite 组件测试与 corpus 差分分别记账。

记录新增 record ID、来源环境、导出日期、Java/C++ revision、脱敏流程版本、artifact SHA-256、
各层完成数和 gate 结论。历史私有 artifact 未保留 SHA、脱敏流程也未版本化，因此明确写为
“未记录”，不补造 digest、不把历史结果称为可独立复现。README、migration plan 和
compatibility contract 统一引用该矩阵，并继续声明：完整生产 corpus、同一 request corpus
的 Java/C++ 差分、生命周期/稳定性验证、灰度与回滚演练全部完成前，不满足生产切流条件。

本项解决的是状态歧义和证据记账，不伪装成实际完成阶段 8。下一份私有差分记录仍必须先
生成稳定 SHA-256 和脱敏版本，再记录项目/route/script/template 数量、未覆盖能力、获批差异、
阶段 gate 与最终切流结论。本项不需要 Fiber 改动、Issue 或 gitlink 更新。

## 13. Fiber 能力核对结果

为了避免不必要的上游修改，本次已核对 pinned Fiber API：

| 能力 | 当前 Fiber 状态 | 结论 |
| --- | --- | --- |
| HTTP socket peer 地址 | `HttpExchange::remote_addr()` 已提供 | trusted proxy 在本项目实现 |
| TLS peer/CA/SNI/验证名 | client context 已支持；pool key 未隔离 transport profile | 进程级模式已接入；路由级等待 Fiber #28 |
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
9. S-03 进程级 upstream peer/CA/SNI 验证模式（已完成；route 级配置移至阶段 C）。

### 阶段 C：Fiber 协同能力

1. L-06 系统 DNS 配置和多 nameserver；
2. P-08 Happy Eyeballs；
3. S-03 route 级 TLS transport profile 隔离与配置；
4. S-04 upstream mTLS client identity；
5. P-01 通用 SWRR 上游化或 sharding；
6. 只有 profile 证明 route pin 成为瓶颈时，开展 P-02 RCU 设计。

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
