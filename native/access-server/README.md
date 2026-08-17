# Access Server

`access-server` 是 Java `../ploto-gateway/ploto-unified-access` 配置和请求执行
能力的 C++23 迁移目标。`../ploto-gateway/unified-access-server` 只作为进程装配和
部署参数参考。

实现直接复用 `third_party/fiber-gateway-cpp` 子模块的 `fiber_lib`、JSON、脚本、
connection pool，以及 Nacos、CAT、Prometheus 组件。迁移不要求这些基础设施与 Java
`fiber-net-gateway` 内部实现兼容；兼容性只在统一接入配置和最终请求结果边界验收。

## 当前状态

当前完成应用脚手架、项目配置 codec、Host/Path 不可变路由快照、RESPONSE 执行内核，
以及 PROXY 请求、普通响应和 WebSocket 101 tunnel：

- CMake 已注册 `fiber_app_access_server`，产物名为 `access-server`；
- 已注册离线 `fiber_app_access_gateway_validator`，产物名为
  `access-gateway-validator`；它复用相同 codec、脚本编译和 compiled model，按版本化
  stdin/stdout JSON 协议为 Console 提供 fail-closed 权威校验，且不连接 Nacos/CAT/公网；
- 已将 native 代码拆为 `access_server_config`、`access_server_observability`、
  `access_server_execution`、`access_server_runtime`、`access_server_validation` 五个显式组件，
  并建立 `fiber_access_server_tests`；
- 已实现项目列表、`ProjectConf`、`HostStrategy`、`RouteItem`、Duration/DataSize 的
  Java 兼容解码；
- 已实现版本化 `AccessConfigLimits`：在解析前限制 payload，在 codec/编译期限制 route、Host、
  header、CIDR、address、script/template、静态响应和 snapshot 预算；超限候选失败保旧；
- 已实现 route build-time 校验、CIDR/address 编译、RESPONSE body 预解码和
  service/cluster 上游计划；
- 已实现 Java 兼容 Host 校验与 exact/wildcard 匹配，并直接复用本仓库
  `RoutePathMatcher` 完成 Path/条件路由选择；
- 已实现跨项目全局 Host 树、候选构建失败保旧、同 version 忽略、Host 为空卸载，
  以及请求对旧不可变快照的 pin；
- 已实现 RESPONSE 的 TEXT/BASE64/TEMPLATE/空 body、静态 TEXT/BASE64 的协商式 gzip、
  受保护响应头过滤、header/body
  原子准备和统一 JSON/HTML 错误结果；项目匹配后的 access-owned header、最终 route/proxy
  header 和 trace header 由请求级 `AccessRequestTelemetry` 统一持有；
- 已实现可直接交给本地 HTTP server 的请求 handler，完成快照 pin、Host/Path/条件
  路由、X-Entry、HTTPS redirect、CIDR、request body limit 和 RESPONSE 串联；
- 已将 PROXY 接入同一 live handler；handler 将 pinned route 和轻量请求上下文直接交给
  executor，由 executor 完成 service/cluster/addresses、method、URI/rewrite/query、Java
  固定 header 过滤、proxy/context/source header、body framing/limit、timeout、flush 和
  WebSocket 请求条件；
- 已实现基于 `StealableHttp1ConnectionPoolSet`/`ClientHttp1Exchange` 的完整 `ProxyExecutor`
  状态机：先选择静态地址或 service endpoint，再构造实际 `Http1RequestHead`，随后查询
  connection pool，并在 miss 后完成 DNS/多地址连接；header/body 流式收发、动态 body
  limit、Java request timeout 和 request header 发送前的连接失败重选均已实现；pool
  lease、discovery generation 与栈上的 upstream exchange 保持到 response/tunnel 结束；
- 已实现普通 upstream response bridge：status/reason/header/body、Java 固定
  hop-by-hop 过滤、自定义响应头模板、Content-Length 特殊恢复、Location/Refresh
  回写、flush 和已知/动态 response body limit；
- 已实现 response header/body 等待期间的 downstream close 竞速、upstream 提前结束
  处理，以及 WebSocket 101 的双向 raw tunnel；101 不经过普通 HTTP body 完成路径；
- 真实 loopback upstream 已覆盖 chunked/Content-Length wire 请求、默认 Host/source
  header、body 字节、service 重选、同一 TCP 连接复用、响应头改写/覆盖、双方提前关闭
  和 WebSocket 双向字节；
- condition 和 `${...}` expression 已在候选快照发布前交给本地 C++ 脚本引擎预编译，
  请求只同步执行不可变程序；支持 `$path/$query/$header/$cookie/$req/$context`，
  编译失败保留上一版配置，通用 Java 脚本兼容不属于本次迁移范围；
- Java golden fixtures 已覆盖未知字段、重复字段、null、标量转型和主要配置字段；
- 已实现 owner-loop Nacos 配置图：项目列表驱动逐项目订阅增删，空/同 version/非法
  更新保留当前路由，列表移除卸载项目，shutdown 等待全部 listener 关闭；
- 已实现独立 TLS 证书快照 watcher：从 leaf DNS SAN 编译 exact/单层 wildcard 索引，
  完整校验候选后原子切换，非法或空候选保留旧快照；ClientHello 热路径不分配、不加锁；
- 已实现 production gray 配置 codec、失败保旧的原子规则快照，以及 CIDR/ratio 对
  NamingService cluster 的覆盖；`context.cluster` 同样会覆盖 route 默认 cluster；
- 已实现 NamingService route 依赖协调、健康/权重/zone/cluster 过滤、不可变服务目录、
  discovery generation pin，以及基于本地 `DnsResolver` 的执行器 DNS adapter；
- 已建立兼容边界、详细配置/请求契约和分阶段验收清单；
- 已实现 `AccessServerRuntime`：启动 Nacos client/config/naming，建立 project/gray
  watcher 和 NamingService selector，在每个 HTTP worker 初始化 DNS resolver 与本地
  connection pool，并在收到项目列表首值后才绑定 listener；
- `AccessServer` 已收敛为 data-plane façade：`AccessWorkerResources` 独占 request handler、
  DNS、connection pool、worker metrics 和 CAT detach 生命周期，`AccessMetricsEndpoint`
  独占 metrics/activation listener；façade 只编排 initialize、bind、serve 和异步 shutdown；
- `main` 已装配 SIGINT/SIGTERM、accept loop、HTTP worker group、CAT sender loop、
  Nacos owner loop 和逆序关闭；关闭顺序为 metrics/业务 listener 与 active exchange、
  指标采集和 CAT worker 上下文、connection pool/DNS、CAT client、配置和服务订阅、
  NamingService、ConfigService、NacosClient；
- 已通过真实 rnacos 验证启动、项目/路由首值、version 热更新和 SIGTERM 退出，并通过
  loopback listener 测试验证 HTTP worker 资源关闭；
- 已实现 Java 测试环境 Host cluster 入口规则：`api_gray.example.com` 以
  `api.example.com` 路由，`gray` 作为请求 cluster，并向上游传递
  `ploto-origin-host`；无 Host cluster 时读取 `HI-TRACE-CLUSTER`；
- 已接入本地 CAT request tree：继续入站 `HI-TRACE-ID`/`HI-SPAN-ID-PARENT`/
  `HI-SPAN-ID`，响应写回 `Hi-Trace-Id`，代理调用生成下一 span；根事务采用 Java
  `URL` 类型和 `<project><route-pattern>` 名称，并记录 project、route、cluster、
  upstream、稳定错误名和最终响应状态；
- 已实现 Java `traceparent`/`tracestate` 传播：缺少 `traceparent` 时生成 sampled W3C
  header；解析 `tracestate` 的 `bnrc` GMP Base62 context 并绑定 `$context`，route
  context 更新后在 upstream 发送前保留其他 vendor member 并重建 `bnrc`；CAT 不可用时
  仍由请求级 telemetry 保持上述传播状态；
- 已接入独立 Prometheus listener，默认 `0.0.0.0:16689`；请求完成计数、inflight、
  duration，以及配置结果/readiness/全局 route snapshot 规模和 age 均使用固定 schema；
  动态 project/route/cluster/Host/Data ID 不作为指标 label，避免配置和请求输入形成无限时序；
- 已在同一运维 listener 上提供默认关闭、Bearer 鉴权的 `/v1/activation-evidence`：按实例报告
  route/project-list/gray/TLS 候选与 active 摘要、不可变快照 generation/fingerprint 和有界
  discovery 聚合；Project 明细使用绑定 evidence revision 的分页游标，配置内容和自由文本错误不出站；
- 已接入共享异步 logging 生命周期，访问日志在 `access_server.access` 以结构化
  key/value 输出 trace、请求、路由、上游、结果、耗时和字节数；队列满时丢弃新日志，
  不反向影响请求执行；
- 2026-07-31 测试环境有限脚本语法快照已完成 352/352 配置 decode + compile-only；完整
  生产 corpus、阶段 8 请求/生命周期/稳定性差分和切流演练尚未完成，当前二进制可用于
  继续联调，但不满足生产切流条件。

首个迁移基线为 `ploto-gateway` commit
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45`。后续若 Java 基线发生变化，应先更新
迁移文档中的基线、fixtures 和差异记录，再移植对应行为。

## 构建

```bash
git submodule update --init --recursive
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_BUILD_TESTS=ON
cmake --build native/build --target fiber_app_access_server --parallel
cmake --build native/build --target fiber_app_access_gateway_validator --parallel
cmake --build native/build --target fiber_access_server_tests --parallel
ctest --test-dir native/build --output-on-failure -L access-server
```

产物位于：

```text
native/build/apps/access-server
native/build/apps/access-gateway-validator
```

离线 validator 可在不生成 Nacos、CAT、Prometheus 和 access-server runtime target 的独立
构建树中构建：

```bash
cmake -S native -B native/build-validator-only -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_BUILD_TESTS=OFF \
  -DACCESS_SERVER_BUILD_RUNTIME=OFF
cmake --build native/build-validator-only \
  --target fiber_app_access_gateway_validator --parallel
```

组件链接方向、源码归属和 validator 独立性门禁见
[`docs/build-boundaries.md`](docs/build-boundaries.md)。

### 离线 Validator

`access-gateway-validator` 读取一条 contract version 1 JSON 请求，payload 使用 basic
Base64，单次协议输入上限为 8 MiB：

```json
{
  "contractVersion": 1,
  "requestId": "request-id",
  "kind": "project_route",
  "project": "example",
  "payloadBase64": "..."
}
```

`kind` 支持 `project_route` 和 `gray_rules`。项目配置会执行完整 codec、Host/Path/CIDR、
模板和 condition 脚本编译；gray 校验额外阻止未知 entry、重复 entry、越界 ratio 和非法
CIDR。结果始终是单条 JSON，包含 `contractVersion`、`valid`、`normalized` 和脱敏的
`errors`，不会回显 payload。

`access-gateway-validator --describe-config-limits` 输出 runtime 与 Validator 共用的 strict
schema version 1 限额 JSON。Console 在启动时探测该输出；完整数值和失败语义见
[`docs/config-resource-limits.md`](docs/config-resource-limits.md)。

## 运行

复制示例配置并至少修改 Nacos 地址：

```bash
cp native/access-server/access-server.env.example access-server.env
./native/build/apps/access-server access-server.env
```

不传参数时默认读取当前目录的 `access-server.env`。`--help` 只打印命令行用法。

进程默认值与安全监听器约束是：

- HTTPS 监听 `0.0.0.0:16688/tcp`，ALPN 提供 HTTP/2 与 HTTP/1.1；
- HTTP/3 在相同地址和端口监听 UDP，并通过 `Alt-Svc` 发布；
- Prometheus `/metrics` 监听 `0.0.0.0:16689`；同一 listener 上的实例证据接口默认关闭，启用时
  必须同时配置稳定实例 ID 和独立 Bearer token；
- HTTP worker 数在启动时根据进程 CPU affinity 和 cgroup v1/v2 CPU quota 自动确定；
- 默认 request body 上限 400 MiB；
- upstream HTTPS 默认保持 Java `legacy_insecure` 兼容模式；可显式切换系统 CA 或挂载的
  自定义 CA bundle 校验；
- upstream hostname DNS 默认在 EventLoop 启动前严格读取 `/etc/resolv.conf`，将最多三个
  nameserver 及 timeout/attempts/rotate 完整注入每个 HTTP worker；不会隐式回退公共 DNS；
- 多地址 upstream 默认使用单一 3 秒 deadline 的 Happy Eyeballs 连接，250 ms 后交错启动另一
  地址族，最多并发 2 个 TCP attempt；一次逻辑连接只持有一个 pool lease；
- 客户端地址默认取 socket peer，忽略所有 forwarding header；
- access log 默认只记录经安全编码的 path，不记录 query value；正常请求默认采样率为
  10000 bps（全量），失败请求始终保留；
- 项目列表 `ploto.unified-access.projects`，route 前缀
  `ploto.unified-access.route.`，group `ACCESS-SERVER`；
- gray data ID `ploto.unified-access.gray-match`；
- Naming/gray group `DEFAULT_GROUP`；service 路由缺省 cluster 固定为 `default`。
- 测试环境 Host cluster 模式默认关闭；仅在明确配置
  `ACCESS_SERVER_TEST_MODE=true` 时启用。

完整键和值示例见 [`access-server.env.example`](access-server.env.example)。配置文件采用
严格的 `KEY=VALUE` 行格式，空行和以 `#` 开头的注释会忽略；重复键和未知键会使进程
启动失败。TLS 与 HTTP/3 默认开启，进程从
`ploto.unified-access.tls-certificates` / `ACCESS-SERVER` 等待首个完整证书快照；缺失、过期、
私钥不匹配或 TCP/UDP 绑定失败时 fail closed。文件证书配置已移除。兼容明文 HTTP 时必须同时
显式关闭 TLS 与 HTTP/3。
`ACCESS_SERVER_DNS_MODE=system`（默认）会从 `ACCESS_SERVER_DNS_RESOLV_CONF` 指定的文件加载
Fiber 有界 resolver 配置；文件缺失、无 nameserver、地址非法或超过三个 nameserver 时启动前
fail closed。`search`/`ndots` 等尚未执行的设置会显式进入固定维度指标，不会被当作已经支持。
`ACCESS_SERVER_DNS_MODE=override` 则要求通过 `ACCESS_SERVER_DNS_SERVERS` 提供 1-3 个逗号分隔的
unicast IPv4/IPv6 literal，端口固定为 53。两种模式均不会添加 `8.8.8.8` 或其他隐式 fallback。
解析结果在进程生命周期内不可变；修改系统 resolver 文件需要滚动重启。

`ACCESS_SERVER_UPSTREAM_CONNECT_TIMEOUT_MILLIS` 是一次 selected-upstream 连接的共享总期限，范围
为 10-60000 ms。`ACCESS_SERVER_HAPPY_EYEBALLS_ENABLED=true`（默认）在 DNS 返回多个地址时调用
Fiber 固定容量 connector；`ACCESS_SERVER_HAPPY_EYEBALLS_DELAY_MILLIS` 范围为 10-2000 ms 且不得
超过总期限，`MAX_CONCURRENT_ATTEMPTS` 范围为 1-4，`FIRST_ADDRESS_FAMILY_COUNT` 范围为 1-16，
地址族策略只能为 `v6_first` 或 `v4_first`。所有 TCP attempt 共用上述期限；首个成功者继续 TLS，
其余 attempt 被取消并关闭。连接池只为整次竞速取得一个 lease，外层 service selection 仍最多执行
Java 兼容的三次 endpoint 尝试。显式关闭 Happy Eyeballs 时保留逐地址串行兼容路径。

`NACOS_SERVER_ADDRESSES` 当前仍要求逗号分隔的 IP literal。Nacos 因而不借用 HTTP worker 的
loop-affine resolver；若未来开放 hostname，必须在 Nacos owner loop 建立并按 service/client 之前
关闭独立 resolver 生命周期。
CAT 默认关闭；任一 `CAT_*` 设置非空后必须给出完整 app key、hostname、IP，以及
至少一个 router 或 bootstrap collector。CAT 不可用会在启动阶段 fail closed，不会
静默退化为无 trace 的生产实例。

`ACCESS_SERVER_UPSTREAM_TLS_MODE` 控制出站 HTTPS 的进程级信任策略：

- `legacy_insecure`（默认）保持 Java `InsecureTrustManagerFactory` 行为，不校验 upstream 证书；
- `system_ca` 使用 Fiber 探测到的系统 CA bundle；
- `custom_ca` 要求 `ACCESS_SERVER_UPSTREAM_TLS_CA_FILE` 指向已挂载的 PEM CA bundle。

安全模式会在 EventLoop 启动前初始化 trust store，路径不存在或 bundle 无效时启动失败，错误和日志
不回显路径。hostname endpoint 自动发送同名 SNI 并校验证书名；IP endpoint 不发送 IP-valued SNI，
而是校验证书的 IP identity。该策略在进程生命周期内固定，因此当前连接池不会跨不同 trust profile
复用连接。CA bundle 内容变更不作为热更新；需要滚动重启以重新验证文件并排空按旧 trust store
建立的连接。路由级 CA、SNI 和独立校验名尚未开放；它们依赖 Fiber 连接池按 TLS transport
profile 隔离，跟踪于
[fiber-gateway-cpp #28](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/28)。

`ACCESS_SERVER_CLIENT_METADATA_MODE` 控制请求来源信任边界：

- `direct`（默认）：client IP 取 socket peer，scheme 取业务 listener 的真实 TLS 状态，忽略
  `Forwarded`、`X-Forwarded-For`、`X-Real-Ip` 和 `X-Forwarded-Proto`；
- `trusted_proxy`：仅当 socket peer 命中 `ACCESS_SERVER_TRUSTED_PROXY_CIDRS` 时解析转发头。
  地址优先级为 `Forwarded`、`X-Forwarded-For`、`X-Real-Ip`，并从右向左剥离可信代理 hop；
- `legacy_headers`：只用于明确的 Java 兼容迁移，保留旧的 header 全信任、非法/缺失
  `X-Real-Ip` 跳过 CIDR 语义，不适用于可被非受信网络访问的 listener。

`trusted_proxy` 必须提供至少一个严格 IPv4/IPv6 CIDR；其他模式配置该列表会启动失败。非法、重复
参数、空 hop、超过 32 hop 或 XFF/XFP 数量错位不会降级读取较低优先级 header，而是回退 socket
peer 和 listener scheme。路由 CIDR、gray、HTTPS redirect、Location/Refresh、CAT 与 access log
共享同一次解析结果。

Access log 的 query allowlist、附加敏感 key、HMAC、成功请求采样率和 path/query 字节上限
分别由 `ACCESS_SERVER_ACCESS_LOG_*` 键配置。默认 allowlist 为空，因此 query value 不会写入
日志。内置敏感 key 不可移除，即使被 allowlist 命中也只输出 `[REDACTED]`；可选 HMAC 使用
实例生命周期内的随机密钥，只用于同一实例内关联，密钥不会进入配置或日志。4xx/5xx、执行
失败、响应未完成和 IO 错误不受成功请求采样率影响。allowlist 对 form-decoded ASCII key
执行大小写敏感的精确匹配，敏感 key 判断则不区分 ASCII 大小写；允许输出的 value 保留原始
query 编码并再次执行日志安全编码，不做 percent decode。

### 同步测试环境 Nacos 配置

`scripts/sync_test_nacos.py` 按 access-server 的实际订阅图导出项目列表、列表引用的
全部 route，以及 gray-match 配置，并可直接发布到测试 rnacos。工具在进程内禁用
HTTP/HTTPS 代理。源地址和凭据由环境变量或参数注入，不在仓库中提供默认值，且源地址、
token 和密码都不会写入 dump 或 manifest。输出目录必须为空，建议放在已忽略的
`temp/` 下：

```bash
ACCESS_SERVER_SOURCE_NACOS_URL='...' \
ACCESS_SERVER_SOURCE_NACOS_USERNAME='...' \
ACCESS_SERVER_SOURCE_NACOS_PASSWORD='...' \
python3 native/access-server/scripts/sync_test_nacos.py \
  --destination-url http://127.0.0.1:18848/nacos \
  --output-dir temp/access-server-nacos-dump
```

同步完成后工具会逐项回读 rnacos 并比较原始内容；`manifest.json` 记录 dataId、group、
字节数和 SHA-256，但不记录 token 或密码。若项目列表引用了不存在的 route，该 dataId
保持 rnacos `NotFound` 状态，并记录在 `missingRoutes` 中。

listener 只在 Nacos client/config/naming、project/gray watcher、项目列表首值，以及 TLS 开启时
首个有效证书快照全部就绪后开放；若项目列表或 TLS 快照不存在，服务会等待到
`ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS` 后失败退出。某个项目的 route 配置尚未
到达或不可用时不会开放对应 Host/Path，请求仍按现有稳定错误结果 fail closed。

## 目录

- `CMakeLists.txt`：应用目标和后续应用内静态库、测试的构建入口；
- `src/main.cpp`：进程配置、loop group、信号和有序关闭；
- `src/config/`：统一接入配置 wire model 和 Java 兼容 codec；
- `src/routing/`：compiled model、CIDR、Host/Path matcher 和全局不可变快照；
- `src/execution/`：live request handler、无状态 Host/Route policy evaluator、RESPONSE
  计划/执行、一次性 PROXY request plan、连接重试与 connected upstream attempt 边界、模板
  适配边界和统一错误响应；
- `src/observability/`：请求 CAT 上下文、固定 schema Prometheus 指标和 logging
  category；
- `src/runtime/`：本地脚本 runtime、候选快照编译/原子发布、Nacos 配置 watcher、
  production gray、NamingService selector、per-worker DNS/pool、HTTP server 和进程
  runtime；
- `src/validation/`：离线 Native Validator 的有界版本协议与权威校验编排；
- `scripts/`：测试环境 Nacos 配置图的无代理导出、rnacos 发布和回读校验工具；
- `tests/`：access-server 聚焦测试和 Java golden fixtures；
- `docs/migration-plan.md`：范围边界、C++ 模块划分、工作包和阶段门槛；
- `docs/compatibility-contract.md`：配置字段、热更新和 HTTP 请求执行的 Java 契约；
- `docs/config-resource-limits.md`：Project List、route、gray 的版本化资源上限和失败保旧语义；
- `docs/config-compilation.md`：route/TLS 专用 compiler loop、generation 合并、owner-loop
  发布和有序关闭契约；
- `docs/config-publication-typestate.md`：Project 候选从 Prepared 到 Ready 再到 commit 的
  move-only 类型状态、取消和兼容语义；
- `docs/bounded-metrics.md`：请求、配置、Nacos/发现、TLS、proxy/DNS/pool/WebSocket、异步日志
  与 CAT 指标的固定 label 集、readiness/snapshot 语义、并发成本及仍待实现的 O-02 范围；
- `../../docs/activation-evidence.md`：实例证据协议、鉴权和分页边界、collector 租约/TTL、
  Release 聚合状态和部署检查；
- `docs/script-corpus-differential.md`：测试环境有限 condition/template/rewrite 快照的证据
  元数据、完成层级、未完成的生产/阶段 8 门禁和私有 corpus 复跑方式；
- `docs/optimization-analysis.md`：代码职责、生命周期、性能、安全和可观测性优化分析，
  以及 Access Gateway 与 Fiber 上游的改造归属。

业务代码开始迁移后，按职责放入 `src/config/`、`src/routing/`、`src/execution/`、
`src/runtime/` 和 `src/observability/`；对应测试放入 `tests/`，并在本目录的
`CMakeLists.txt` 中注册。

## 迁移原则

- Java 配置字段、默认值、宽松输入、Nacos data ID/group、路由优先级和错误结果
  属于外部兼容契约，未经明确决定不改名、不折叠；
- 配置更新先完整解析和校验，再以不可变快照发布；请求不能混用新旧配置；
- 热路径遵循本仓库的内存与异步约束，不按 Java 对象模型逐类机械翻译；
- 不把通用脚本语法、connection pool 算法或监控客户端内部行为纳入迁移验收；
- 每一阶段先增加聚焦测试，再接入下一层运行时依赖；
- 缺失或无效的控制面数据不得产生可用路由；切流前仍需完成阶段 7/8 的观测和差分门槛。

详细范围与阶段见 [`docs/migration-plan.md`](docs/migration-plan.md)，字段和请求契约见
[`docs/compatibility-contract.md`](docs/compatibility-contract.md)。
