# Access Server 代码结构与 API 设计复核

## 1. 文档目的和审阅边界

本文记录 2026-08-18 对 `native/access-server` 的一次代码结构、class 职责、API 参数/返回值、
错误传播、构建契约和资源边界复核。它是对
[`optimization-analysis.md`](optimization-analysis.md) 的当前 API/结构补充，不替代 Java
兼容契约、配置发布 typestate 或生产切流证据文档。

审阅依据包括：

- `native/access-server/src/` 的头文件和实现；
- `native/access-server/CMakeLists.txt`、runtime/validator 构建边界；
- focused tests、benchmark 基线和现有优化记录；
- 离线 Native Validator 的 Release 构建和 `--describe-config-limits` 输出。

本次没有连接 Nacos、CAT、公共网络或生产服务，也没有完整生产脚本 corpus 和 Java/C++ 请求级
差分，因此本文不宣称生产兼容或切流条件已经满足。生产 corpus、阶段 8、逐实例激活、灰度和
回滚证据仍以 [`script-corpus-differential.md`](script-corpus-differential.md) 和
[`cutover-evidence-gate.md`](cutover-evidence-gate.md) 为准。

## 2. 总体结论

`access-server` 不需要推倒重来。当前运行时已经有较清楚的 control plane/data plane 分工、
不可变路由快照、worker-local pin、Prepared/Ready 类型状态和有序生命周期。最值得优先处理的
是“边界和契约的一致性”，而不是在没有 profile 的情况下替换 HostMatcher、SWRR 或连接尝试
协程。

问题按优先级分为：

- **P0**：会阻断可复现构建或使依赖契约失效；
- **P1**：会造成跨层耦合、状态误读、错误丢失或失败路径不可靠；
- **P2**：主要影响长期维护、编译时间和后续演进，应由 profile/规模数据触发。

## 3. 已有的良好设计

### 3.1 结构和生命周期

- CMake 对 config、validation、observability、execution、runtime 和测试源文件采用显式列表，
  没有使用 glob；见 [`CMakeLists.txt`](../CMakeLists.txt#L10) 和测试注册
  [`CMakeLists.txt`](../CMakeLists.txt#L255)。
- `AccessRuntimeFactory` 负责组装，`AccessControlPlaneSupervisor` 负责配置/发现/TLS/CAT
  生命周期，`AccessDataPlaneService` 负责 listener、script runtime 和 worker resources，
  `AccessRuntimeCoordinator` 负责状态机和逆序关闭。这种分工已经足够支撑后续增量改造。
- `AccessServer` 只保留 initialize、bind、serve 和 shutdown facade；worker 侧资源集中在
  [`AccessWorkerResources`](../src/runtime/AccessWorkerResources.h#L48)。不应为了“更面向对象”
  再引入请求路径上的继承树或共享所有权。

### 3.2 快照和兼容性

- `ProjectRouteSnapshot`/`AccessRouteSnapshot` 在完整编译后才发布，请求 pin 住不可变快照，
  失败候选可以保留旧快照。
- `PreparedProjectUpdate -> ReadyProjectUpdate -> commit` 的 move-only typestate 能把服务发现
  readiness 约束在提交边界，相关 API 见 [`RouteConfigStore.h`](../src/runtime/RouteConfigStore.h#L61)。
- 同版本忽略、空 Host unload、项目移除、请求 pin 生命周期和 bounded metrics 等兼容语义已有
  测试和文档，不应被结构重构顺手改变。

## 4. 需要调整的地方

### 4.1 P0：Fiber revision、CMake target 和构建说明不一致（已解决）

2026-08-18 的评审环境中曾发现：

- [`native/access-server/CMakeLists.txt`](../CMakeLists.txt#L50) 只检查 Nacos、CAT、Prometheus
  target 是否存在；
- 但 execution target 在 [`CMakeLists.txt`](../CMakeLists.txt#L91) 无条件链接
  `fiber::http_compression`；
- 当前子模块工作树检出 `abc8c34ba13bd50554a55e10389c6b3da2dcc048`，而仓库 gitlink 和
  [`UPSTREAM.md`](../UPSTREAM.md#L12) 记录的是 `8e8e1d7933d4a30aa3b21feb6acc3e633a612b9b`；
- 因此完整 runtime configure/regenerate 失败，离线 validator 因 runtime 被跳过而仍可构建。

这是可复现构建问题，不应通过修改 submodule 内部文件解决。2026-08-19 已完成以下收口：

1. gitlink 和 [`UPSTREAM.md`](../UPSTREAM.md#L12) 统一固定为
   `0df9dd0d533c96555653af9288faeb54964359bc`；
2. 已审阅 `3d4b350..0df9dd0`，增量仅修改未由本项目构建的 lite-nginx launcher shutdown；
3. Release native configure、默认目标构建、337 项 access-server focused CTest 和 1,977 项完整
   CTest 均通过，5 项按环境条件 skip。

因此当前 pin 的 runtime 可复现构建阻断已经解除。对全部 runtime 必需 target 增加统一的
configure-time 缺失诊断，以及继续按 [`build-boundaries.md`](build-boundaries.md#L81) 收窄
`PUBLIC`/`PRIVATE` 依赖，仍可作为后续 build hardening，但不再阻断当前依赖版本。

### 4.2 P1：收窄 control plane 与 data plane 的接口

`AccessServer`、`AccessWorkerResources` 和 `AccessDataPlaneService` 当前都接收
`RouteConfigStore`，例如 [`AccessDataPlaneService.h`](../src/runtime/AccessDataPlaneService.h#L45)。
但请求处理器实际只需要两指针的 `AccessRouteSnapshotProvider`
（[`AccessRouteSnapshot.h`](../src/routing/AccessRouteSnapshot.h#L56)）。

建议让 data plane 依赖一个只读、冷路径组装的 binding（以下为 API 形态示意）：

```cpp
struct AccessDataPlaneDependencies {
    AccessRouteSnapshotProvider routes;
    ProxyClusterMatcher gray_matcher;
    const AccessRuntimeMetrics* runtime_metrics = nullptr;
    const AccessActivationEvidenceStore* activation_evidence = nullptr;
};
```

这样可以隐藏 compiler、registry、publisher，避免 data plane 获得可变 `RouteConfigStore`，同时
保留当前无锁的 worker-local 快照 pin。`void*` adapter 仍可保留以避免热路径成本，但应由非空
factory/构造函数统一创建，并明确 context 的生命周期。

`RouteConfigStore` 目前同时提供候选 prepare、service selector binding、registry 和 publisher
编排。短期不必增加大量 public class；长期可把“候选准备”和“已编译快照提交”分成两个内部
组件，使 store 名称与职责更准确。

### 4.3 P1：用显式 disposition 替代 optional/bool/status 组合

当前存在多组容易产生非法组合的返回模型：

- [`AccessConfigCompiler.h`](../src/runtime/AccessConfigCompiler.h#L31) 的
  `optional<version> + optional<snapshot> + compilation_skipped`；
- [`RouteConfigStore.h`](../src/runtime/RouteConfigStore.h#L37) 的 status、nullable snapshot
  和独立的 `published` bool；
- [`ProjectConfigCompiler.h`](../src/routing/ProjectConfigCompiler.h#L29) 的
  `expected<optional<ProjectRouteSnapshot>>`。

建议只在 Java wire/codec 边界保留 `nullopt` 的兼容含义，内部改用显式结果（以下为类型形态示意）：

```cpp
struct ProjectCompileRequest {
    std::string_view project;
    std::string_view content;
    std::optional<std::int32_t> published_version;
    CompileMode mode = CompileMode::Normal;
};

using ProjectCompileOutcome =
        std::variant<EmptyCandidate, UnchangedCandidate, CompiledCandidate>;

enum class UpdateEffect {
    Ignored,
    Published,
    Unloaded,
    Removed,
};
```

`ConfigBatchUpdateResult::published` 应由 typed outcome 或 snapshot generation 推导，不再作为
可与 status 冲突的独立字段。watcher 的 generation、version、Data ID、MD5、时间戳也应封装为
`ProjectUpdateMetadata`，减少长参数函数的错配风险。

另一个明确问题是 [`AccessDnsService::init`](../src/runtime/AccessDnsService.h#L64) 返回
`Task<bool>`；调用方无法区分 cache、resolver、worker entry 或配置失败，当前还会把 false 统一
映射为 `IoErr::NoMem`。建议返回 `expected<void, AccessDnsInitError>`，在 runtime 边界再转换为
稳定的 `AccessServerRuntimeError`。

### 4.4 P1：统一 options，避免默认值和构造路径漂移

body limit 在不同 options 中分别出现 400 MiB、0 和 4 MiB，见
[`AccessServer.h`](../src/runtime/AccessServer.h#L25)、
[`AccessDataPlaneService.h`](../src/runtime/AccessDataPlaneService.h#L28) 和
[`AccessRequestHandler.h`](../src/execution/AccessRequestHandler.h#L45)。
`RoutePolicyEvaluator` 对 0 有特殊语义，直接构造和生产 factory 构造因此可能得到不同限制，
甚至落入无限制路径。

建议：

- 定义唯一的 `kDefaultMaxRequestBodySize`，并把“Java 默认值”“部署覆盖值”“unlimited”分成
  不同命名状态；
- 将 options 拆成 `AccessRuntimeLoops`、`AccessRequestPolicyOptions`、
  `AccessDataPlaneDependencies`、`ListenerOptions` 等聚合；
- 用聚合对象替代 [`AccessServerConfig`](../src/runtime/AccessServerConfig.h#L98) 的长位置参数
  构造函数和 runtime factory 的长参数列表；
- 对 `AccessControlPlaneSupervisor` 使用 validated dependency aggregate 或静态 `create()`。

当前 supervisor 构造函数在 body 内断言之前，已经在 member initializer 中解引用
`config_service_` 和 `naming_service_`，见
[`AccessControlPlaneSupervisor.cpp`](../src/runtime/AccessControlPlaneSupervisor.cpp#L57)。
空依赖会先触发未定义行为，不能依赖构造后的 `FIBER_ASSERT` 修复。

不同层使用 `IoResult`、`expected<..., AccessServerRuntimeError>`、`Task<void>` 是可以接受的，
但建议定义领域别名并明确转换边界，避免用 `bool` 或无错误的 `Task<void>` 隐藏失败原因。

### 4.5 P1：`noexcept` 分配、发布顺序和全局资源预算

以下 cold-path API 声明为 `noexcept`，但会分配或复制：

- [`RouteSnapshotPublisher::publish`](../src/runtime/RouteSnapshotPublisher.h#L31) 会为每个 worker
  创建 wrapper；
- `AccessActivationEvidenceStore::publish` 会复制 evidence、构造 fingerprint；
- watcher 的 activation evidence 会创建 vector、string 和 set。

当前 native 编译使用 `-fno-exceptions`，因此 OOM 无法通过普通异常传播。应二选一：

1. 把可能失败的分配前移到 prepare，publish 只执行已准备好的不可失败交换；或
2. 明确 OOM 是 fatal policy，并在文档、指标和测试中体现，而不是让接口看起来可恢复。

`RouteConfigStore::commit_batch` 当前先替换 registry，再调用 publisher；如果未来 publisher
需要返回可恢复错误，可能形成 registry 与 published snapshot 不一致，见
[`RouteConfigStore.cpp`](../src/runtime/RouteConfigStore.cpp#L384)。

另外，单项目限制不能替代全局限制。`AccessRouteSnapshot::build` 当前累加 project、host、route、
program 和 estimated bytes，但没有统一的 aggregate budget，见
[`AccessRouteSnapshot.cpp`](../src/routing/AccessRouteSnapshot.cpp#L22)。建议增加总 project/host/
route/program/bytes 上限，并对累加使用 checked addition。

`Err` 也建议改为紧凑的 tagged union：当前
[`AccessResult.h`](../src/execution/AccessResult.h#L79) 同时保留 `IoErr` 和 `Exception` 字段，
存在无效组合并增加请求路径对象大小。

### 4.6 P2：大编排器和执行接口的内部收敛

`ProjectConfigCompiler.cpp` 和 `AccessConfigWatcher.cpp` 均超过 1,200 行，
`AccessControlPlaneSupervisor.cpp` 约 500 行。这里不建议立即拆成很多 public service；建议先
按 private pipeline/translation unit 拆分：

- compiler：wire normalization、semantic validation、route type compiler、template/script
  compiler、budget checker、snapshot assembler；
- watcher：subscription graph/reconcile、compile queue/generation、service-ready、readiness/
  evidence；
- supervisor：保留生命周期 owner，内部只下沉 typed context 和 factory。

执行层还有几个低风险 API 改进点：

- [`ProxyRequestPlan`](../src/execution/ProxyRequestPlan.h#L52) 的构造函数和 `prepare()` 重复接收
  exchange/proxy，建议改成一次性 `build(context, endpoint)`，之后只允许 `rebind_endpoint()`；
- [`ProxyAddressSelector`](../src/routing/ProxyAddressSelector.h#L94) 同时承载请求选址、readiness
  和 service metadata，建议分离 request selector 与 control-plane binding；
- [`AccessRequestHandler::handle`](../src/execution/AccessRequestHandler.h#L74) 接收两个可能不匹配
  的对象，建议使用绑定 exchange 的 `RequestContext`；
- `ProjectRouteSnapshot.h` 中公开的 `CompiledResponseRoute`、`CompiledProxyRoute`、`CompiledRoute`
  是可构造的 mutable aggregate，建议改为 detail 类型、私有构造或只读 view；
- activation evidence 内部状态应使用 enum，序列化到 API 时再转字符串；trace state 的 `parse()`
  若需要诊断 malformed input，建议返回 `ParseDisposition` 或保留可观测的 invalid 标记。

### 4.7 P2：配置契约文档需要统一版本命名

代码和 validator 已使用 `schemaVersion = 2`，但资源限制文档、README 和兼容文档仍有
“schema version 1”的表述；validator 的 `contractVersion = 1` 又是另一层协议版本。
涉及文件包括：

- [`AccessConfigLimits.h`](../src/config/AccessConfigLimits.h#L56)；
- [`config-resource-limits.md`](config-resource-limits.md#L7)；
- [`README.md`](../README.md#L188)；
- [`server/src/integrations/native-validator/model.ts`](../../../server/src/integrations/native-validator/model.ts#L3)。

建议统一命名为 `limitsSchemaVersion` 与 `validatorContractVersion`，并同步 server、Console、
fixtures 和用户文档，避免控制面与数据面对同一个版本号产生不同解释。

## 5. 推荐落地顺序

1. 对齐 Fiber gitlink/CMake target，恢复完整 runtime 的可复现 configure/build；
2. 统一 schema 版本命名和 body-limit 默认常量；
3. 引入 data-plane dependency bundle、snapshot source、typed options 和非空依赖 factory；
4. 将 compile/update 结果改为显式 disposition，修正 DNS 和 lifecycle 错误传播；
5. 处理 publish/OOM 语义并增加 aggregate snapshot budget；
6. 只拆 private compiler/watcher pipeline，不改变已验证的请求热路径布局；
7. 在取得生产更新频率和内存 profile 后，再决定 coalescing 或增量全局索引。

每一步都应保留以下回归：同版本忽略、无效候选保旧、空 Host unload 与项目移除的区别、请求
快照 pin、selector readiness、取消/关闭顺序、bounded metrics 和 secret redaction。

## 6. 本次验证记录

- `cmake --build native/build-validator-only --target fiber_app_access_gateway_validator --parallel 4`：通过；
- `apps/access-gateway-validator --describe-config-limits`：输出 `schemaVersion: 2`；
- `cmake --build native/build --target fiber_access_server_tests --parallel 4`：未完成，CMake
  regenerate 因缺少 `fiber::http_compression` target 失败；
- validator-only build tree 没有注册 focused tests；完整 runtime CTest 因 configure 阻塞未运行。

评审过程中没有修改 `native/access-server` 源码，也没有修改 Fiber 子模块。工作树中已有的
`third_party/fiber-gateway-cpp` 子模块变更保持原样。
