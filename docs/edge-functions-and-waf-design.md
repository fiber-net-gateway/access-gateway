# 边缘函数与 WAF 设计

- 状态：Proposal（方向已评审，未实施）
- 日期：2026-11-05
- 适用范围：`native/access-server/`（边缘函数执行钩子）、`server/`（WAF 规则模型 + 代码生成器 + 发布资源）、`web/`（WAF 管理页）
- 分层结论：**数据面不理解 WAF**。数据面只提供通用的"边缘函数"钩子；WAF 是控制面的第一个编译目标，规则在发布前被组合并生成一段 Fiber 脚本，作为普通边缘函数下发执行。

## 1. 背景与分层决策

需求是在网关上提供请求侧检测与处置能力（WAF 是第一个消费者），规则主要用脚本引擎表达。有两种分层方式：

| 维度 | 数据面懂 WAF（规则信封进 wire） | 数据面只懂函数（本设计） |
| --- | --- | --- |
| 数据面 API 面 | rule/severity/scope/action/exclusion 全套语义进 codec、limits、validator、watcher | 一个通用钩子：挂载的函数列表 + 执行契约 |
| rnacos 契约 | `waf-rules` Data ID 携带规则结构，规则模型每次演化都是跨组件变更 | 通用函数索引 + 每函数脚本 Data ID，与 WAF 终身解耦 |
| 能力上限 | 只能表达规则能表达的事 | 边缘函数是独立能力；限流、请求改写等将来都是"控制面前端 + 生成器" |
| 复用度 | 新造一套规则执行管线 | 复用现有 route script 全部基础设施（`req.*`/`resp.*`、每请求 GcHeap、编译管线） |

采用第二种的实质理由：控制面拥有全部策略语义，`effective = f(rule, project mode)` 这类合成在发布期完成，数据面零策略；这与仓库既有哲学一致——"publication deterministically compiles its model into the native wire format"（web 路由 YAML → JSON wire 是先例）。

WAF/边缘函数是全新能力，**没有 Java wire 兼容包袱**：错误码、Data ID、metrics、日志类别都使用自有命名空间，不占 Java 兼容词汇。

## 2. 术语

- **边缘函数（edge function）**：挂在某组项目上、在 host 匹配之后执行的 Fiber 脚本。可提前结束请求，也可放行。
- **函数索引（functions index）**：wire 上的 JSON 清单（§3.1），列出函数及其脚本 Data ID、内容 hash 与挂载条目。
- **函数脚本（function script）**：每个函数一个独立 Data ID 的纯脚本文本。
- **WAF 规则**：控制台的结构化模型（§4.1），不进 wire。发布时由**代码生成器**编译为一段边缘函数脚本。

## 3. 数据面：边缘函数契约

### 3.1 rnacos 契约：索引 + 每函数脚本 Data ID

沿用 projects list → per-project route 的既有"索引 + 每条目独立 Data ID"模式：

| 用途 | Data ID | Group | 形态 |
| --- | --- | --- | --- |
| 函数索引 | `ploto.unified-access.edge-functions` | `ACCESS-SERVER` | JSON，versioned |
| 函数脚本 | `ploto.unified-access.edge-function.<functionId>` | `ACCESS-SERVER` | 纯脚本文本（TEXT） |

索引示例：

```json
{
  "version": 3,
  "functions": [
    {
      "id": "waf-generated",
      "name": "WAF",
      "dataId": "ploto.unified-access.edge-function.waf-generated",
      "sha256": "…hex…",
      "mounts": [
        { "projects": { "include": ["*"], "exclude": ["loan-admin-legacy"] }, "priority": 100 }
      ]
    }
  ]
}
```

- `mounts[].projects` 为 project 名 glob 列表（include/exclude），纯挂载语义，无策略含义。
- 项目级 WAF mode（off/log/block）**不设 wire 字段**：由生成器折叠——mode 为 `off` 的项目直接进 `exclude`（零执行成本），`log`/`block` 项目共享同一脚本，脚本头部按 `$ctx.project` 查表。
- 脚本 Data ID 的身份 = 索引中的 `sha256`；脚本文本不变则 hash 不变，watcher 编译产物按内容寻址复用（改挂载只 bump 索引版本，不重写脚本）。

**组装语义（failure-old-snapshot 的推广）**：快照切换**全或无**。watcher 收到索引后逐一校验引用脚本：本地缓存命中（同 hash 已编译）则复用，否则订阅/拉取 → 校验 sha256 → 受限编译；全部就绪才组装联合快照并原子替换，任一脚本缺失、hash 不匹配或编译失败 → 保留旧快照 + 失败计数 + 日志。索引同版本忽略；索引中消失的函数 Data ID 成为孤儿，直接不参与组装（删除清理由发布侧负责，数据面不做删除依赖）。

**发布顺序约束**（控制面必须遵守）：先写全部脚本 Data ID 并 readback 成功，最后写索引——反向顺序会出现"索引指向不存在内容"的窗口，触发上面的一致性保持而停留在旧快照。

### 3.2 执行点与语义

管线锚点（`AccessRequestHandler::handle_impl`）：

```
gray matcher → host match → host policy（X-Entry / HTTPS 强制）→ 【边缘函数】→ prepare_constants → route match → …
```

放在 host policy 之后：入口网络边界策略（entry/https）先于应用层逻辑生效，保持确定性；边缘函数异常不影响网络边界判定。放在 route match 之前：**未命中任何路由的路径同样被保护**（扫描行为常打不存在路径）。未命中任何 host 的请求维持现状直接 `router_not_found`（无项目可定位、无策略可施）。

执行语义：

1. host→project 解析后取该项目挂载的函数列表（快照索引），按 `priority` 升序、同序按索引内声明序执行。
2. 每个函数是独立编译产物，**共享该请求已有的每请求 GcHeap**（`ScriptExecutionContext`，串行 `exec_sync` 合法；无新增 per-request 分配）。
3. **短路判定零新协议**：函数执行后检查 `ScriptExchangeCtx::response_header_sent()`（已存在，`AccessScriptRuntime.cpp` 已在用）——已发送即终结请求，不执行后续函数与路由；未发送则继续。
4. 未挂载函数的项目零成本（索引为空，不进入任何脚本路径）。
5. 函数异常/中止：fail-open（§6）。

### 3.3 v1 执行契约（写死，只增不收）

| # | 契约 | 实现方式 |
| --- | --- | --- |
| 1 | 只读请求：`$req`/`$header`/`$query`/`$cookie`/`$conn`/`req.*` 可读；不可改写 path/method/header/query | 不注册任何改写函数；v1 无改写 API |
| 2 | 提前响应：`resp.*` 全量可用 | 复用现有 route script 语义 |
| 3 | 事件上报：仅 `ctx.report(event, label)` | 新增宿主函数，见 §3.5 |
| 4 | CIDR 集合：`directive x = cidr "…"; x.contains(ip)` | app 侧脚本扩展，见 §3.4 |
| 5 | 不消费 body：`req.readJson/readBinary/discardBody` 不可用 | 前置函数使用**受限 Library** 编译：不注册 body 读取函数。函数名为编译期静态解析，未注册即编译失败——fail-closed |
| 6 | 无上游访问：不注册 `http` directive | 同上，函数不能发起上游请求、不能变代理 |
| 7 | 无函数间通信、无可变上下文 | 无对应 API |

契约 5/6 的实现要点：为边缘函数构建专用 `StdLibrary`（在 `AccessScriptCompiler` 旁），注册 `req.*` 读函数、`resp.*`、`ctx.report` 与 cidr directive 扩展；`$req/$header/$query/$cookie/$conn` 常量复用 `RouteScriptExtension`，另加 `$ctx = {project, host}` 常量组（客户端 IP 直接用已有的 `$conn.remote_addr`）。编译一律 `allow_assign=false`。**cidr directive 只在边缘函数 Library 开放，不进 route condition**——避免路由配置语义漂移。

### 3.4 cidr directive

方言无位运算，IP/CIDR 判断通过 directive 注入为原生能力（`StdLibrary::add_ext_ops` 是公开应用侧扩展点，`AccessScriptCompiler` 已用同机制组合两个扩展；`DirectiveDef` 虚接口在公开头文件——**零 third_party 改动**）：

```javascript
directive office = cidr "172.28.1.0/24;10.164.44.2" "32.43.5.46/32";

// 规则体内：
if (!office.contains($conn.remote_addr) && strings.match($req.uri, "(?i)union\\s+select")) {
    ctx.report("waf.block", "sqli-0023");
    resp.send(403, "blocked");
}
```

- **声明**：`directive <name> = cidr "<cidr;cidr;…>" "<更多>";`——一个或多个字符串字面量，内部分号分隔，空段忽略；声明必须在编译单元顶部（先声明后调用）。
- **编译期**：宿主在 `resolve_directive_def` 中解析全部 CIDR（复用 `routing/Cidr.h`，与 `allows`/GrayMatchStore 同款）并构建不可变匹配器；**非法 CIDR 字面量 = 编译失败 = 发布期 fail-closed**。
- **运行期**：`contains(ip)` 为纯 C++ 区间匹配——无 VM 循环、无堆分配、无逐次解析。方法集 v1 仅 `contains(string) -> boolean`。
- **软失败语义**：`null`（`$conn.remote_addr` 转换失败时为 null）、空串、畸形 IP → `false`。注意两个方向的含义：allowlist 场景 false = 不在名单（偏保守）；**blocklist 场景 false = 放行**——规则作者必须意识到畸形 IP 的行为。
- CIDR 变化 = 脚本文本变化 = 新 hash = 重新发布重编译，全部发生在配置期，无运行期成本。
- limits：每 directive CIDR 数上限（对齐 `max_cidrs_per_route = 256`）、每脚本 directive 数、字面量字节上限，进 `EdgeFunctionLimits`。

### 3.5 `ctx.report(event, label)`

唯一新增的"策略性"原语，语义刻意通用：

- 参数均为字符串；每次调用产生一条**事件计数**与一条**结构化日志**。
- 计数进入 bounded 动态 LRU 家族（照 `native/access-server/docs/bounded-metrics.md` 的既有模式，标签值请求期到达、家族容量上限 + 逐出）：
  `access_server_function_events_total{function, event, label}`。
- WAF 的 rule id / action 就作为 `label`/`event` 传入——数据面不知道也不需要知道这是规则。
- 基数防护：每函数每请求 `report` 调用次数上限（limits，建议 16，超限静默丢弃 + 溢出计数器）；家族 LRU 上限沿用 bounded-metrics 约定。

### 3.6 配置编译、快照与联合索引

- 新增 `EdgeFunctionsWatcher`（照 `AccessConfigWatcher`/`GrayConfigWatcher` 模式）：订阅索引 + 引用的脚本 Data ID → 校验 hash → 逐函数受限编译 → 按 §3.1 的全或无语义组装。编译产物按脚本内容（sha256）寻址缓存，未变函数复用。
- **联合索引**：`project → 函数列表` 的 glob 展开不能在请求期做。挂载展开依赖 projects list 与函数索引两个版本，在快照装配侧（`AccessConfigCompiler`/`RouteSnapshotPublisher` 一侧）于任一变更时重算，请求期只做一次查表。
- limits 新增 `EdgeFunctionLimits`（挂进 `AccessConfigLimits` 并提升 `schema_version`）：索引 payload 上限、函数数上限、单函数脚本字节数、挂载条目数、编译产物总大小估算、每函数 report 次数、cidr directive 相关上限（§3.4），全部进 validator `--describe-config-limits` schema。

### 3.7 观测

- metrics（命名对齐 `access_server_*` 既有风格）：
  - `access_server_function_evaluations_total{function, project}`（项目 label 走 bounded 动态家族）
  - `access_server_function_duration_seconds`（histogram，function label）
  - `access_server_function_errors_total{function, reason}`（Exception/Abort/Timeout/OversizeReport）
  - `access_server_function_events_total{function, event, label}`（§3.5）
  - `access_server_function_config_failures_total`（索引/脚本组装失败）
  - `access_server_functions_configured`（gauge，当前快照函数数）
- **访问日志 19 字段 Java 契约不动**。函数事件（含短路）写独立结构化日志行：requestId/traceId/project/host/function/event/label + 截断的目标片段。
- CAT：v1 不改事务模型，trace id 已在事件日志中（事务标记形态列为开放项 §8）。

## 4. 控制面：WAF 编译器

### 4.1 规则模型（控制台内部模型，不进 wire）

```yaml
rules:
  - id: sqli-0023              # 稳定 ID，进日志/metrics 标签/豁免引用
    name: "union select 注入探测"
    severity: high             # critical/high/medium/low，仅元信息
    priority: 100              # 生成 if 链顺序
    action: block              # log | block
    phase: request_head        # v1 仅此枚举
    match: |                   # 表达式，与路由 condition 同语言同编译管线
      strings.match($req.uri, "(?i)union[+%20\\s]+select")
      || strings.match($query.q, "(?i)union\\s+select")
    scope:                     # 折叠进生成脚本
      methods: [GET, POST]
      paths: ["/api/*"]
      source_cidrs: ["172.28.0.0/16"]   # 生成器收集去重 → 脚本顶部 cidr directive 声明
    exclusions:
      - project: loan-web
        reason: "内部报表导出含 SQL 片段, 2026-11-05 张三"
projectWaf:
  loan-admin: {mode: log}      # off/log/block；off 的项目在生成时进挂载 exclude
```

脚本做不了的事由信封/生成器补：CIDR 判断经 `scope.source_cidrs` 语法糖编译为脚本顶部的 cidr directive 声明（多条规则共享的网段去重合并），规则体引用 `<name>.contains($conn.remote_addr)`；手写场景（P3）可直接写 directive，两者兼容。性能敏感的全量扫描由 `scope.methods/paths` 折叠为 `strings.hasPrefix`/`contains` 前置链（公共前缀合并，不逐规则跑正则）。需要完整脚本表达力的场景走既有 Script 路由类型，不往 WAF 规则里塞。

校验阶段对齐现有分层：字段语法（类型/长度/枚举/ID 唯一性）→ 交叉校验（豁免引用的规则 ID 存在、scope 字段与 CIDR 合法）→ 全环境校验（**生成产物必须通过 native validator 编译**，fail-closed，Release 准备之前）。

### 4.2 代码生成器（`server/`，关键路径组件）

- 输入：规则集 + 项目 mode 表；输出：单函数脚本文本（含顶部 cidr directive 声明）+ 索引条目（exclude 含全部 mode=off 项目）。
- 生成策略：priority 升序 if 链；mode 查表在脚本头部；`block` 命中 = `ctx.report("waf.block", "<ruleId>")` + `resp.send(403, …)` 默认 JSON 体 `{code:"WAF_BLOCKED", ruleId, requestId, traceId}`；`log` 命中 = 仅 `ctx.report("waf.log", "<ruleId>")`。
- **转义器**是安全关键点：规则里的正则/字面量要安全转义进方言字符串。方言转义与 JS 有差异（无 `\u{...}`、支持 1-3 位八进制、`\a` 写出普通 `a` 等），转义器单独单测并覆盖恶意输入（转义错误 = 规则语义偏移 = 安全漏洞）。CIDR 列表同样经转义器进 directive 字面量。
- **黄金 fixtures**：规则集 → 生成脚本快照对比（对齐 `tests/fixtures/java` 的做法），并用 native validator 断言全部 fixture 编译通过。
- 发布预览展示生成脚本文本（可 diff），编译错误行号回映规则 ID——可调试性必做项。

### 4.3 server 模块与发布

- `modules/waf`：规则 CRUD（draft 态）、组合与生成、发布；数据库新增 waf 草稿/发布记录（schema migration，PR 中列明）。
- 发布资源条目 = **函数索引 Data ID + N 个函数脚本 Data ID**，各自独立记录 write/readback/failure/evidence（多资源发布框架已支持）；**先写脚本并 readback 成功，最后写索引**（§3.1 顺序约束）。脚本文本未变时跳过该 Data ID 的写入（hash 相同）。回滚 = 从历史 release 内容（索引 + 脚本）重新写入发新版本，不改写历史。
- validator 扩展：函数索引 schema + 每函数离线编译（含 cidr directive 解析），`--describe-config-limits` 同步；server 侧 fail-closed 语义照旧（`NATIVE_VALIDATOR_PATH`）。
- 激活证据：watcher 上报 applied generation（索引版本 + 各脚本 hash），接入现有逐实例 activation collector；证据缺位时 UI 显示 unknown，不以 rnacos 写入/读回推断成功。

### 4.4 web

- WAF 页：规则表格 + 结构化编辑 + `match` 表达式 CodeMirror 编辑器 + 即时编译校验（server → validator）。
- 项目策略页：per-project mode 与豁免管理（豁免带原因，可审计）。
- 发布预览：生成脚本 diff + 编译结果。
- 边缘函数页：**v1 占位只读**（展示当前函数索引与脚本内容），手写函数 P3 开放（见 §7）。

## 5. 性能预算

- 挂载项目每请求：1 次 `exec_sync`（GcHeap 已存在，无新增分配）+ 生成代码的前置字符串链；CIDR 判断为纯 C++ 区间匹配，无 VM 参与。
- 目标：典型 50 规则集函数求值 p99 < 100µs；进 `benchmark:native` 基线，PR 中报告。
- 未挂载项目零成本；mode=off 项目经挂载排除零成本。
- 热路径承诺：数据面侧短路判定（`response_header_sent()`）与索引查表无分配。

## 6. 安全与失败语义

- **fail-open**：函数 Exception/Abort → 放行 + `function_errors_total` 计数 + 日志（WAF 挂了不能打死网关）。严格模式（fail-close）留作 P3 配置项。
- 输入截断：进入脚本的每个字符串字段截取前 N KiB（默认 8KiB，limits 可调），防长 header/URI 拖慢求值。
- **VM 无 step budget**（`InterpreterVm` 无指令预算；`ScriptAbortReason::Timeout` 仅有枚举位）。v1 缓解：方言静态封堵（无函数定义/递归/`while`，唯一循环是 for-of over 有限容器）+ 表达式/脚本/产物上限 + 输入截断。根治手段是**上游建议**：InterpreterVm 增加可选 step budget/deadline；顺带建议上游更新 `docs/script-guide`（`strings.match/findAll` 在当前 pin 已注册，文档仍写"未注册"）。
- 正则：`strings.match` 为 RE2 线性实现 + 编译缓存 + WorkBudget，无回溯炸弹。CIDR 经 directive 编译期解析，畸形 CIDR 在发布期失败。
- 生成器安全：转义器单测 + 黄金 fixtures + validator fail-closed 三层兜底。
- 部署依赖与顺序：**fleet 需先升级到 pin ≥ 4a5f12b8**（`strings.match/findAll` 依赖；111/218/219 目前在旧 pin）。新二进制在无函数索引时零行为变化——先升二进制（ctl upgrade，窗口 <2ms），再发布首个函数索引 + 脚本，灰度顺序：单项目 log 模式 → 观察 → block。

## 7. 阶段计划

| 阶段 | 内容 |
| --- | --- |
| P0 | fleet 升级至 pin ≥ 4a5f12b8；上游建议（step budget、script-guide 文档） |
| P1 native | `EdgeFunctionLimits` + 索引/脚本 codec + validator 扩展 → `EdgeFunctionsWatcher`（hash 校验 + 内容寻址编译缓存 + 全或无组装）+ 联合索引 → 受限 Library（含 cidr directive 扩展）+ 编译 → 管线钩子 + 短路 → `ctx.report` + metrics + 事件日志 → CTest 覆盖（含黄金 fixture 的 native 侧编译断言、cidr directive 编译期失败用例） |
| P2 server+web | `modules/waf` + 转义器/生成器 + 黄金 fixtures + 发布资源条目（先脚本后索引）+ 激活证据；WAF 页 + 项目策略页 + 发布预览；边缘函数只读页 |
| P3 | body phase（缓冲-重放、per-route opt-in、content-type 白名单、大小上限——219 大文件分块上传的前车之鉴，绝不全量缓冲）；手写边缘函数开放（先扛住 step budget / 基数上限硬约束）；fail-close 选项；起手规则包（SQLi/XSS/路径穿越/扫描器 UA/畸形 URI，纯数据）；控制台命中统计页（server 集成 Prometheus 查询） |

## 8. 开放项

- CAT 事务标记形态（函数短路是否独立 transaction type 或仅 trace 标记）。
- 函数能否读 trace context（`$context` 已含 trace cluster；开放给 `$ctx` 的范围待实现时定）。
- 响应阶段函数（`phase: response`）与请求改写（`phase: request_rewrite`）的契约扩展——远期，先留枚举位不实现。
- 限流作为第二个编译目标的可行性（依赖每 worker 状态与时间窗原语，数据面需新增受控 API）。
