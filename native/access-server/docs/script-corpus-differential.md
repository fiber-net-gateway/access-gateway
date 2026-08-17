# 测试环境有限 condition/template/rewrite corpus 差分记录

> 记录状态：`PARTIAL`。本记录证明 2026-07-31 测试环境快照中的有限脚本语法可被当前
> C++ 实现接受，并覆盖一组脱敏请求样例；它不等于完整生产 corpus、阶段 8 全量请求差分
> 或生产切流验收。最终切流门禁状态为 `NOT_MET`。

## 1. 记录身份与证据边界

| 字段 | 记录值 |
| --- | --- |
| Record ID | `script-syntax-test-2026-07-31` |
| 来源 | 测试环境完整配置图，不是生产环境全量导出 |
| 导出日期 | 2026-07-31 |
| Java 参考基线 | `ploto-gateway` `22c2bf543b96b52c0ccecd4ceb07d4911c502f45` |
| C++ 执行 revision | 历史执行未单独记录；本记录首次随 `5ced45cb2c1fae2b848b98ec580a7af06e5fd86e` 入库 |
| 脱敏流程版本 | 历史执行未记录版本；不得据此声明脱敏流程可复现 |
| 私有 corpus SHA-256 | 历史执行未记录，且原始 artifact 不在仓库中；不得补造 digest |
| 仓库内证据 | 聚焦 C++ 测试、本文统计和已脱敏期望值 |
| 最终切流资格 | `NOT_MET` |

缺失执行 revision、artifact digest 和脱敏版本不会推翻当次测试结果，但意味着该历史记录
不能独立满足可复现 corpus 门禁。下一份 corpus 记录必须在执行前生成 record ID、脱敏流程
版本和只读输入集合的稳定 SHA-256：在私有环境中按相对路径排序，为每个脱敏文件记录
SHA-256，再对 UTF-8 manifest 本身计算 SHA-256；仓库只记录最终 digest，不提交可能含业务
标识的 manifest。原始业务内容继续只保存在受控私有存储中。

## 2. 范围与数据边界

本轮以 2026-07-31 导出的测试环境完整配置图作为有限语法快照，只比较
`ploto-unified-access` 实际使用到的 condition、template 和 rewrite 行为：

- Java 参考实现：
  `RouteExecutionBuilder.compileExpression/parseTemplate`、
  `ScriptStringTemplate` 和 `ConditionalExecution`；
- C++ 实现：
  `AccessScriptCompiler`、`AccessScriptRuntime`、`TemplateEvaluator`、compiled route snapshot
  和请求执行计划；
- 不把 Nacos 地址、账号、密码、项目名、Host、业务 header value、表达式字符串常量
  或完整 route JSON 写入 Git；
- 原始 dump 和 Java 探针产物只保留在被忽略的 `temp/` 中。

这只是有限的测试环境语法兼容，不扩大为 Java/C++ 通用脚本 VM 等价承诺，也不代表
生产配置、生产流量或阶段 8 生命周期场景已经覆盖。

## 3. 快照覆盖

| 项目 | 数量 |
| --- | ---: |
| 项目 route 配置 | 352 |
| RouteItem | 1302 |
| condition | 11 |
| 含表达式的 template value | 810 |
| template expression segment | 811 |
| condition + template expression segment | 822 |
| rewrite string | 330 |
| 其中含表达式的 rewrite | 311 |
| 纯字面量 rewrite | 19 |
| 由 Jackson scalar coercion 得到的 rewrite | 1 |

810 个含表达式 template value 的位置分布：

| 位置 | 数量 |
| --- | ---: |
| `proxy_headers` | 482 |
| `response_headers` | 10 |
| `context` | 6 |
| `rewrite` | 311 |
| TEMPLATE response body | 1 |

实际语法集中在：

- `$path.tail/tail2/t/env`；
- `$header.host/hi_trace_cluster/origin/...`；
- `$context.hi_trace_cluster`；
- `$req.path/query/method`；
- `$query.redirect`、`$cookie.cluster`；
- `||` 默认值、`==`/`!=`/`<=`；
- `strings.hasPrefix` 和 `rand.random`。

所有 `$path.*` 引用都能在各自 route pattern 中找到对应 capture；该快照的 template
没有反斜杠转义、未闭合表达式或异步表达式。

## 4. 证据矩阵

| 证据层级 | 本记录结果 | 可复现性 | 门禁结论 |
| --- | --- | --- | --- |
| 外部配置 decode + compiled snapshot | 历史运行 352/352 通过 | 私有输入和 SHA 未入库；本地提供同一 corpus 时可重跑 | 仅 compile-only 完成 |
| Java 脱敏 golden | 14 个 template case、5 个 condition case 通过 | Java probe 原始产物未入库 | 有限语法样例完成 |
| C++ 请求级 condition/template | `MatchesRecordedConditionAndTemplateSyntaxSnapshot` 通过 | 仓库内可重跑 | 一个聚合脱敏场景完成 |
| C++ rewrite 行为 | 通用 route/proxy 测试覆盖编译、模板和最终 request target | 仓库内可重跑，但没有独立的 production rewrite corpus 测试 | 有限组件覆盖；非 corpus 全量差分 |
| 完整生产配置 corpus | 未执行 | 无可审计 artifact | 未完成 |
| Java/C++ 同一 request corpus 全量差分 | 未执行 | 无统一 request record | 未完成 |
| 阶段 8 热更、慢 body、断连、超时、shutdown | 聚焦测试存在，但未以同一 Java/C++ corpus 验收 | 无阶段 8 记录 | 未完成 |
| 性能、连接复用、内存和 fd 稳定性 | 未形成切流记录 | 无阶段 8 记录 | 未完成 |
| 灰度、最小线上演练和立即回滚 | 未执行 | 无演练记录 | 未完成 |
| 最终生产切流 gate | 上述必需项未全部完成 | — | `NOT_MET` |

这里的“352/352”只指配置 decode 和 compiled snapshot 构建，不能用于推断 352 个项目都
完成了请求级 Java/C++ 差分。单元测试或测试环境 listener 结果也不能替代生产配置证据、
受控上游场景、稳定性基线和回滚演练。

## 5. 差分方法

### 5.1 外部配置 compile-only

`ProductionScriptCorpusTest.CompilesExternalSnapshotWhenProvided` 从外部目录读取 route
JSON，逐份执行 C++ wire decode、route matcher 构建和脚本预编译。测试只报告配置序号
与结构化错误，不输出文件名或配置内容：

```bash
ACCESS_SERVER_SCRIPT_CORPUS_DIR=/path/to/private-dump/routes \
./native/build/access-server/fiber_access_server_tests \
  --gtest_filter=ProductionScriptCorpusTest.CompilesExternalSnapshotWhenProvided
```

未设置 `ACCESS_SERVER_SCRIPT_CORPUS_DIR` 时该测试跳过，因此 CI 不依赖私有数据。

历史快照结果：352/352 配置完成 decode 和 compiled snapshot 构建。由于该次执行没有记录
私有输入 SHA-256，本结果是不可独立复现的历史证据；它不能升级为完整生产 corpus gate。

### 5.2 Java golden 与 C++ 请求级执行

基于上述语法集合构造脱敏固定输入，由 Java 参考类执行 14 个 template case 和 5 个
condition case。C++ 仓库内的请求级聚合用例为：

- `AccessRequestHandlerTest.MatchesRecordedConditionAndTemplateSyntaxSnapshot`。

rewrite 的编译、模板渲染和最终 upstream request target 由
`AccessRequestHandlerTest.PassesPinnedProxyRouteAndExecutionInputToAdapter` 与
`ProxyExecutorTest.StreamsJavaCompatibleRequestsAndReusesTheUpstreamConnection` 等通用用例
覆盖；当前没有名为 `MatchesProductionRewriteCorpus` 的独立 corpus 用例，因此不把这些
组件测试记作 production rewrite corpus 全量差分。

随机表达式使用 `rand.random(1) <= 0` 固定结果，只验证函数调用、数字比较和 condition
truthiness；不比较 Java/C++ PRNG 序列。

## 6. 差异与修复

首次差分发现 `$context.hi_trace_cluster` 不兼容：

- Java `ConstPackage` 对变量名执行 ASCII 大小写折叠，并把 `-` 归一为 `_`，所以表达式
  key `hi_trace_cluster` 能读取运行时 `HI-TRACE-CLUSTER`；
- C++ 原实现只接受 `cluster`、`HI_TRACE_CLUSTER` 和 `HI-TRACE-CLUSTER` 的精确拼写，
  导致现网 36 个 `$context.hi_trace_cluster` 引用得到 null，并错误使用 `||` fallback。

C++ 现由共享 `ConstPackage::Builder` 只收集这些动态名称常量，在编译期执行同等归一化，
并在请求执行前按 package index 填充 context 槽位；固定的 `$req`/`$conn` 由 exchange
extension 直接读取，不改动共享脚本 VM。
修复后，当次记录中的 Java golden、condition/template 请求结果和两类 rewrite 结果一致。

## 7. 当前结论与后续记录要求

- 该测试环境快照的全部 condition/template/rewrite 可被 C++ 配置入口接受并预编译；
- 已观察到的变量、默认值、比较、prefix 判断、template 文本转换和 rewrite URI 行为一致；
- 编译失败仍在候选快照发布前 fail closed，并保留上一成功版本；
- 后续配置新增未覆盖的 namespace、函数或转义形态时，必须先更新脱敏 corpus 和 golden，
  不能仅以“脚本引擎不同”跳过差分；
- 完整生产 corpus、同一 request corpus 的 Java/C++ 全量差分、阶段 8 生命周期/性能验证、
  灰度与回滚演练仍未完成，因此当前明确不满足生产切流条件。

后续每份差分记录必须包含：导出日期、来源环境、Java/C++ revision、脱敏流程版本、私有
artifact 的稳定 SHA-256、项目/route/script/template 数量、各证据层完成数、未覆盖能力、
获批差异、阶段 gate 结论和最终切流结论。缺失项必须写 `未记录` 或 `未完成`，不得从相邻
测试结果推断为已通过。
