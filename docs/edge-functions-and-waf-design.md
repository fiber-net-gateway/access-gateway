# 边缘函数与 WAF 设计

- 状态：P1（native 数据面）、P2（server/web WAF 控制面，e5925f7）、P3 ③fail-close（908a1d2）、④起手规则包（ca691d8）、⑤命中统计页（a093da2）、②手写边缘函数开放（草稿存储 8404ea0 + 多函数发布 969c214 + 控制台 70b26ad）均已实施；剩 P3 ①body phase。2026-11-06 修订（slug `name` 身份 + 挂载级 `ratio` 采样）已随 P2 落地。 2026-11-07 修订（挂载拍平到函数条目 + `replaces` 版本灰度 + 抽签键改用解析后的客户端地址，limits schemaVersion 6）同日落地。
- 日期：2026-11-05（修订：2026-11-06）
- 适用范围：`native/access-server/`（边缘函数执行钩子）、`server/`（WAF 规则模型 + 代码生成器 + 发布资源）、`web/`（WAF 管理页）
- 分层结论：**数据面不理解 WAF**。数据面只提供通用的"边缘函数"钩子；WAF 是控制面的第一个编译目标，规则在发布前被组合并生成一段 Fiber 脚本，作为普通边缘函数下发执行。

## 1. 背景与分层决策

需求是在网关上提供请求侧检测与处置能力（WAF 是第一个消费者），规则主要用脚本引擎表达。有两种分层方式：

| 维度          | 数据面懂 WAF（规则信封进 wire）                                                   | 数据面只懂函数（本设计）                                                        |
| ------------- | --------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| 数据面 API 面 | rule/severity/scope/action/exclusion 全套语义进 codec、limits、validator、watcher | 一个通用钩子：挂载的函数列表 + 执行契约                                         |
| rnacos 契约   | `waf-rules` Data ID 携带规则结构，规则模型每次演化都是跨组件变更                  | 通用函数索引 + 每函数脚本 Data ID，与 WAF 终身解耦                              |
| 能力上限      | 只能表达规则能表达的事                                                            | 边缘函数是独立能力；限流、请求改写等将来都是"控制面前端 + 生成器"               |
| 复用度        | 新造一套规则执行管线                                                              | 复用现有 route script 全部基础设施（`req.*`/`resp.*`、每请求 GcHeap、编译管线） |

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

| 用途     | Data ID                                             | Group           | 形态               |
| -------- | --------------------------------------------------- | --------------- | ------------------ |
| 函数索引 | `ploto.unified-access.edge-functions`               | `ACCESS-SERVER` | JSON，versioned    |
| 函数脚本 | `ploto.unified-access.edge-function.<functionName>` | `ACCESS-SERVER` | 纯脚本文本（TEXT） |

**身份字段方案（2026-11-06 修订定稿）**：函数条目的身份收敛为单一 slug 字段 `name`，不再有独立的 `id` 与线上 `dataId` 字段。理由：`dataId` 恒等于前缀 + slug，属可派生冗余，派生后"dataId 不匹配"这个错误类从契约中整个消失；id/name 双字段（slug 身份 + 可空展示名）迫使发布侧每次分辨"用哪个"；展示名（中文、空格、任意文本）是控制台 DB 的职责，不进 wire。收敛后 wire 条目 = 作者格式（temp 授权文件与线上一一对应，发布工具零映射）。代价是两条写死的契约：

1. **`name` 是 slug，不是显示名**：它流入脚本 Data ID、metrics `function` label、事件日志与激活证据，字符集必须封死（见下表）。
2. **`name` 是承重身份，没有"原地重命名"**：改名 = 删除旧函数 + 新建新函数（旧脚本 Data ID 成孤儿由发布侧清理、metrics 时间序列在旧名下终止、激活证据标签换名）。

索引示例：

```json
{
  "version": 3,
  "functions": [
    {
      "name": "waf-sqli",
      "sha256": "…64 位小写 hex…",
      "projects": { "include": ["*"], "exclude": ["loan-admin-legacy"] },
      "priority": 100
    },
    {
      "name": "bot-trap",
      "sha256": "…64 位小写 hex…",
      "projects": { "include": ["*"] },
      "ratio": 1000
    },
    {
      "name": "auth-v1",
      "sha256": "…64 位小写 hex…",
      "projects": { "include": ["*"] },
      "priority": 200
    },
    {
      "name": "auth-v2",
      "sha256": "…64 位小写 hex…",
      "projects": { "include": ["loan-*"] },
      "priority": 200,
      "ratio": 300,
      "replaces": "auth-v1"
    }
  ]
}
```

字段规格（codec 逐字段强制，**未知字段一律拒绝**，fail-closed）：

顶层：

| 字段        | 类型    | 必填 | 约束    | 语义                                                                                       |
| ----------- | ------- | ---- | ------- | ------------------------------------------------------------------------------------------ |
| `version`   | integer | 是   | ≥ 0     | 发布侧单调递增的索引版本；数据面不比较大小，仅透传进激活证据；与 limits schemaVersion 无关 |
| `functions` | array   | 否   | ≤ 64 条 | 省略与空数组等价 = 卸载全部函数；JSON 根为 `null` 同义                                     |

函数条目（`functions[i]`）：

| 字段               | 类型            | 必填 | 约束                                                                   | 语义                                                                                                     |
| ------------------ | --------------- | ---- | ---------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `name`             | string          | 是   | `[a-z0-9-]` 1-64 字节，首尾必须字母数字，索引内唯一（重复 = conflict） | slug 身份；脚本 Data ID = `ploto.unified-access.edge-function.` + `name`，watcher/validator 派生，不上线 |
| `sha256`           | string          | 是   | 恰 64 个小写十六进制字符                                               | 对发布写入的脚本文本字节（UTF-8）计算；内容寻址编译缓存的键；watcher 逐脚本校验                          |
| `phase`            | string          | 否   | `"request_head"`（缺省）或 `"request_body"`，其它拒绝                  | 执行阶段（P3 ①，已实施）：head 阶段脚本引用 `$req.body` 编译失败（fail-closed）；body 阶段语义见 §3.2    |
| `projects`         | object          | 是   |                                                                        | 项目名选择器                                                                                             |
| `projects.include` | array\<string\> | 是   | ≥ 1 条，≤ 256 条                                                       | 项目名 glob；先命中 include，再被 exclude 否决                                                           |
| `projects.exclude` | array\<string\> | 否   | ≤ 256 条，默认 `[]`                                                    | 否决名单                                                                                                 |
| `priority`         | integer         | 否   | [0, 1000000]，默认 0                                                   | 升序执行；同序按索引声明序（稳定）                                                                       |
| `ratio`            | integer         | 否   | [0, 10000]，默认 10000                                                 | 采样比例（万分比）：缺省 = 必跑；< 10000 = 粘性采样；0 = 挂载保留但禁用（见下"挂载采样"）                |
| `replaces`         | string          | 否   | 目标 slug；约束见下"版本灰度"                                          | 版本灰度：本函数在 `ratio` 比例内取代目标函数，两者互斥执行（见下"版本灰度"）                            |

**挂载拍平（2026-11-07 修订）**：一个函数只有一组项目选择器、一个 `priority`、一个 `ratio`，它们直接写在函数条目上。此前的 `mounts[]` 只有一个独特价值——同一函数在不同项目用不同比例或优先级，WAF 生成器固定只产一条、手写函数未见使用，而 `projects.include/exclude` 本身已能选多个项目；代价是 first-match 语义和一层嵌套。`maxMountsPerFunction` 随之删除。不再能表达"已全量某项目、其余项目 3%"的二维阶梯：要么先按项目扩（比例 10000），要么全项目同比例。

glob 规则：非空可打印文本、不含 `;`（项目列表 wire 以分号分隔，项目名里不会有它）、单条 ≤ 255 字节；通配语法为单星 `*`（带回溯匹配），无 `?`/`**`/字符类。

**挂载采样（`ratio`，2026-11-06 修订新增）**：缺省/10000 = 必跑（作者视图的 `must`）；< 10000 = 按比例采样执行（作者视图的 `maybe`，比例 = `ratio`/10000）；0 = 挂载保留但禁用（订阅与证据不断，比卸载再重挂温和）。采样判定为**粘性单次抽签**：请求期至多计算一次 `crc32(客户端 IP 原始字节) % 10000`（CRC-32，zlib 多项式/Java zip 同构，`fiber::util::Crc32`，v4/v6 字节数组直算），该请求命中的全部采样函数用**同一抽签值**对比各自的桶区间，落在区间外即跳过该函数（计入 `function_sampled_out_total`，不执行、不进 GcHeap）。粘性带来两个必然性质：同一客户端行为一致（block 灰度期不会一半请求拦一半放）；阈值单调（ratio 从 1000 提到 2000 只增不减，原命中者继续命中，指标前后可比）。快照内该项目命中的函数列表全部必跑时不抽签——纯必跑 fleet 零额外成本。

**抽签键（2026-11-07 修订）**：抽签用解析后的 `client_address`，无解析结果时退回 socket 对端。`direct` 模式下两者相同；经 CDN 或 LB 接入的实例必须把 `ACCESS_SERVER_CLIENT_METADATA_MODE` 配成 `trusted_proxy` 或 `legacy_headers`，否则抽签仍落在中间节点上：对端只有少量 IP 时比例严重失真（`ratio=300`、16 个连续对端 IP 均匀分流的模拟中，灰度完全没流量的概率约 61%，最高可到 37.5%），用户换节点时还会在版本间跳动。切换此键会让既有 ratio 函数的命中人群重新洗牌一次。`legacy_headers` 下客户端能伪造地址头的入口，抽签同样可被操纵。

**版本灰度（`replaces`，2026-11-07 修订新增，取代原"互斥分群留 v2"）**：`replaces` 声明本函数取代哪个函数，目标条目**完全不改**。组装快照时按项目计算互斥的半开桶区间：在两者都挂载的项目上，灰度函数占 `[0, ratio)`，目标让出这段、保留 `[ratio, 目标 ratio)`；其余项目目标不受影响。上例 `loan-*` 上 `auth-v2` = [0,300)、`auth-v1` = [300,10000)，其他项目 `auth-v1` 全量。性质：

- **绝不双跑**：两个区间不相交，请求只跑其中一个；提高 `ratio` 时用户只会单向迁到新版本。
- **回滚 = 删除或禁用灰度函数**：目标从未被改动，移除后行为完全还原。
- **只收窄、不扩大**：目标保留自己的 `priority`、`include`、`exclude`，未挂载目标的项目上未命中时什么都不跑。这也是不采用"未命中时执行替代函数"的原因——那会把目标带到它被明确排除的项目，且要求目标摘掉自己的挂载（否则跑两次），上灰度和回滚都得同时改两个函数。
- **不是异常兜底**：灰度函数报错仍走 fail-open/fail-close，不回退到目标，所以字段不叫 `fallback`。
- 端点都合法：`ratio` 0 = 全走目标（已上线、不放量），10000 = 全走新版本、目标留作即时回滚（窗口为空，不抽签也不执行，计入 `sampled_out`）。

codec 约束（`replaces` 需要全索引，解析完全部条目后校验；违反 = 整个索引拒绝、保留旧快照）：目标必须在索引内、不能指向自己、`phase` 必须相同、目标自身不能再声明 `replaces`（不支持链）、一个目标只能被一个函数取代（否则多个灰度争抢同一段低位桶，又回到双跑）。声明顺序无关。身份上新旧版本是两个函数（`name` 决定脚本 Data ID），转正后名字与指标序列随之变化；控制台提供"创建灰度版本"（`auth` → `auth-v2`，1% 起步）与"转正"，后者只在 `ratio` = 10000 且挂载范围与目标一致时可用，此时目标已不执行任何请求，删除它是纯清理、不改变流量。

已知限制：所有采样函数共用一个桶，同时进行的多个灰度落在同一批低位桶的客户端上，彼此完全相关，出问题难归因（需要时按函数名加盐，代价是每个函数一次 CRC）；WAF 生成函数不能作为目标（控制台校验拒绝），规则组改动的灰度仍靠 log/block 模式；body 阶段的采样判定在缓冲之后，低比例的 body 函数也让全部 opt-in 请求付缓冲成本。

**作者格式与 wire 格式分离**（对齐 web 路由 YAML → JSON wire 的既有先例）。P3 ② 实施后控制台的作者格式是**函数优先草稿**（环境级单例：`functions[]` = `name`/`description`/`enabled`/`phase`（缺省 `request_head`，P3 ①）/`script`/`projects`（`include`/`exclude` glob）/`priority`/`ratio`/`replaces`），发布时由 server 合并 WAF 生成条目、按 `priority`+名称排序、对脚本内容计算 sha256、套上 `version`；wire 只认函数优先 + 严格 JSON（无注释、未知字段拒绝）。早期设想的 mount-first 作者视图（`temp/edge-functions.json` 的 `must`/`maybe` 条目）被该草稿模型覆盖，其 `must` ≡ 缺省 `ratio` 10000、`maybe` ≡ `ratio < 10000`，表达力等价。编译预览与发布校验都**按各自 phase** 走原生 validator（`edge_function_script` 请求带 `phase` 字段，缺省 = request_head）：head 阶段引用 `$req.body` 在预览即失败，不会拖到发布或运行期。

脚本 Data ID（每函数一个）：内容为纯脚本文本，sha256 必须与索引一致（不一致 = 组装失败，全或无保留旧快照）；空串/NotFound = 该函数保持当前（keep-current），与索引的 keep-current 语义对齐。limits：索引 payload ≤ 256 KiB、单脚本 ≤ 1 MiB、快照内全部脚本合计 ≤ 8 MiB。

limits 同步修订：`maxFunctionIdBytes` 删除（身份只剩一个），`maxFunctionNameBytes` 128 → 64（从展示名上限变为 slug 上限），`--describe-config-limits` schemaVersion 3 → 4；`ratio` 为固定值域 [0,10000]（同 `priority` 的 [0,1000000]），不设 limits 字段。P1 已提交的 codec/validator/server 镜像（master `23c2388`）仍是修订前形态（`id` + 可空 `name` + `dataId`、无 `ratio`），本修订的实现变更为下一步（未部署、P2 未消费，零迁移）。body phase（P3 ①）追加 `edgeFunctions.maxBodyPhaseBytes`（缺省 1 MiB），schemaVersion 4 → 5。 挂载拍平（2026-11-07）删除 `maxMountsPerFunction`，schemaVersion 5 → 6；`replaces` 复用 slug 约束，不设 limits 字段。

- 项目级 WAF mode（off/log/block）**不设 wire 字段**：由生成器折叠——mode 为 `off` 的项目进其所在各组的 `exclude`（零执行成本），`log`/`block` 项目共享组脚本，block 规则命中后按 `$ctx.project` 判定。
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
2. 采样在执行前判定（§3.1 挂载采样）：命中的函数列表存在任一非全量函数时，请求期计算一次 `crc32(客户端 IP 原始字节) % 10000` 并与各函数的桶区间比较，落在区间外 = 跳过该函数（计入 `function_sampled_out_total`，不执行、无 GcHeap 成本）；全部必跑时不抽签，零额外成本。桶区间在快照组装时算好（`ratio` 与 `replaces` 都折叠进去），请求期不解析函数间关系。
3. 每个函数是独立编译产物，**共享该请求已有的每请求 GcHeap**（`ScriptExecutionContext`）。执行用 `exec_async`（`resp.*` 含异步 opcode，`exec_sync` 遇异步指令会 panic）；纯同步函数的 `exec_async` 不挂起，成本与 `exec_sync` 相同。无新增 per-request 分配。
4. **短路判定零新协议**：函数执行后检查 `ScriptExchangeCtx::response_header_sent()`（已存在，`AccessScriptRuntime.cpp` 已在用）——已发送即终结请求，不执行后续函数与路由；未发送则继续。
5. 未挂载函数的项目零成本（索引为空，不进入任何脚本路径）。
6. 函数异常/中止：fail-open（§6）。

**body 阶段管线锚点（P3 ①，已实施）**：`request_body` 函数在 route match 命中后、代理执行前运行，且仅当**该路由为 PROXY 且配置 `edge_function_body: true`**（route YAML 字段，wire `edge_function_body` 可空 Java bool）。门前依次为：

1. 项目未挂载任何 body 阶段函数（快照双链：head 链在路由前、body 链在路由后，`mounted_body_for`）——零成本，不读交换状态；
2. 路由非 PROXY（`not_proxy`）或未 opt-in（`route_opt_out`）——跳过，不触碰请求体；
3. content-type 白名单（`content_type`）——固定代码表 v1：`application/json`、`application/x-www-form-urlencoded`、`text/plain`、`application/xml`、`text/xml`（按 `;` 截参数、去空白、ASCII 大小写不敏感）；无 content-type 头同样跳过；
4. 声明式 Content-Length 超上限（`content_length`，`maxBodyPhaseBytes`）——跳过，不读一个字节；
5. 流式读取超过上限（`stream_overflow`）——**保留已缓冲前缀、跳过函数**，前缀 + 未读尾部照常拼进上游请求：缓冲对无上限上传也有界（219 大文件分块上传的前车之鉴，绝不全量缓冲）；未溢出则函数以 `$req.body` 常量（原生字符串零拷贝视图）求值，缓冲副本随后重放进代理（prefetch 链先于交换读被消费，预算/完整性分类与真实读一致）。

全部跳过原因进 `access_server_function_body_skipped_total{project, reason}`（动态序列 LRU 2048），缓冲字节数进 `access_server_function_body_buffered_bytes_total`。预读用与代理 body 管道相同的空闲超时（`max(60s, route timeout)`），慢涓流不会钉死 worker 槽。空 body + 命中白名单 content-type 仍执行函数（`$req.body` 为空串）。

### 3.3 v1 执行契约（写死，只增不收）

| #   | 契约                                                                                                                                                                                  | 实现方式                                                                                                                                                                                                                                                                                                                                                                                                                           |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | 只读请求：`$req`/`$conn`/`req.*` 函数族（`getHeader/getQuery/getCookie/getUri/getPath/getQueryStr/getMethod`）与 `$header/$query/$cookie` 常量可读；不可改写 path/method/header/query | 不注册任何改写函数；v1 无改写 API。`$header/$query/$cookie` 自 Fiber 动态常量模型（pin ≥ 7ee40abe：无 prepare、按请求经 `ScriptExchangeCtx::resolve_constant` 解析并备忘）起在边缘函数可用——组合 `RouteScriptExtension`（空 path vars、禁 http directive）即得，**值不截断**（`req.*` 函数族仍按 `max_input_field_bytes` 截断，规则内长值匹配请用常量形式）。`$context`（项目级 trace 上下文）与 `$path`（无路由已选）保持编译失败 |
| 2   | 提前响应：`resp.*` 全量可用                                                                                                                                                           | 复用现有 route script 语义                                                                                                                                                                                                                                                                                                                                                                                                         |
| 3   | 事件上报：仅 `ctx.report(event, label)`                                                                                                                                               | 新增宿主函数，见 §3.5                                                                                                                                                                                                                                                                                                                                                                                                              |
| 4   | CIDR 集合：`directive x = cidr "…"; x.contains(ip)`                                                                                                                                   | app 侧脚本扩展，见 §3.4                                                                                                                                                                                                                                                                                                                                                                                                            |
| 5   | 不消费 body：`req.readJson/readBinary/discardBody` 不可用；`$req.body` 常量仅 body 阶段存在                                                                                           | 前置函数使用**受限 Library** 编译：不注册 body 读取函数。函数名为编译期静态解析，未注册即编译失败——fail-closed。P3 ① 起 Library 按 phase 编译：body 阶段把 `$req.body` 注册为指向缓冲副本的零拷贝原生字符串常量；head 阶段不注册（引用即编译失败，与 fail-closed 同构），编译缓存键 = `<sha256>:<phase>`                                                                                                                           |
| 6   | 无上游访问：不注册 `http` directive                                                                                                                                                   | 同上，函数不能发起上游请求、不能变代理                                                                                                                                                                                                                                                                                                                                                                                             |
| 7   | 无函数间通信、无可变上下文                                                                                                                                                            | 无对应 API                                                                                                                                                                                                                                                                                                                                                                                                                         |

契约 5/6 的实现要点：为边缘函数构建专用 `StdLibrary`（在 `AccessScriptCompiler` 旁），注册 `req.*` 读函数、`resp.*`、`ctx.report` 与 cidr directive 扩展，并组合 `RouteScriptExtension`（空 path vars、`http_directives_enabled=false`）提供 `$header/$query/$cookie` 动态常量；`EdgeFunctionProgram` 持有 `shared_ptr<const ConstPackage>` 作生命周期锚（脚本的 constant 引用指向包内存储，包须与脚本一同长寿）。常量键只接受标识符字符集——**写规范化形式（小写、`-` 写作 `_`），运行期经 normalize 匹配真实 header/cookie/query 名**；连字符会按减法解析成未定义全局算术，运行期 TypeError（编译不报错，属已知脚枪，规则作者需用规范化拼写）。`$req`/`$conn` 复用 `ExchangeConstExtension`，`$ctx = {project, host}` 为新增运行期常量组（客户端 IP 直接用 `$conn.remote_addr`）。`$context` 仍为项目路由专属（app 侧 `TraceContextConstExtension` 经每请求 services 指针读 trace 状态），边缘函数不开放。编译一律 `allow_assign=false`。**cidr directive 只在边缘函数 Library 开放，不进 route condition**——避免路由配置语义漂移。

**方言脚枪（P3 ① 实施期实证，规则作者须知）**：`contains` 中缀运算符在 `if (...)` 条件内**语法不合法**（parser 报 unexpected token，head/body 阶段一致，属既有文法非 bug）；文本匹配的规范写法是函数形式 `strings.match($req.uri, "…")` / `strings.match($req.body, "…")`（RE2，`(?i)` 等内联 flag 可用）。`resp.addHeader(name, value)` 为驼峰拼写（`add_header` 未注册）；且 setHeader/addHeader 只在脚本**自行 send** 时才上线路——代理路径上脚本未短路时这些挂起 header 被丢弃，改写响应头需 v2 契约显式设计。

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
- limits：每 directive CIDR 数上限（`max_cidrs_per_directive = 1024`；初版对齐 `max_cidrs_per_route` 的 256，2026-11 提升，路由/灰度侧维持 256）、每脚本 directive 数、字面量字节上限，进 `EdgeFunctionLimits`。

### 3.5 `ctx.report(event, label)`

唯一新增的"策略性"原语，语义刻意通用：

- 参数均为字符串；每次调用产生一条**事件计数**与一条**结构化日志**。
- 计数进入 bounded 动态 LRU 家族（照 `native/access-server/docs/bounded-metrics.md` 的既有模式，标签值请求期到达、家族容量上限 + 逐出）：
  `access_server_function_events_total{function, event, label}`。
- WAF 的 rule id / action 就作为 `label`/`event` 传入——数据面不知道也不需要知道这是规则。
- 基数防护：每函数每请求 `report` 调用次数上限（limits，建议 16，超限静默丢弃 + 溢出计数器）；家族 LRU 上限沿用 bounded-metrics 约定。

### 3.6 配置编译、快照与联合索引

- 新增 `EdgeFunctionsWatcher`（照 `AccessConfigWatcher`/`GrayConfigWatcher` 模式）：订阅索引 + 引用的脚本 Data ID → 校验 hash → 逐函数受限编译 → 按 §3.1 的全或无语义组装。编译产物按脚本内容（sha256）寻址缓存，未变函数复用。
- 观察语义（与项目路由对齐）：索引空串/NotFound = 保持当前（codec 契约），卸载只走 JSON `null` 或空 `functions` 数组；**字节相同的索引重发 = 无操作**（同 md5 路由同 doctrine，不产生新代）；脚本订阅启动失败无定时重试——**重发索引（即使字节相同）即重驱动该 Data ID 的订阅**。
- **联合索引**：`project → 函数列表` 的 glob 展开不能在请求期做。挂载展开依赖 projects list 与函数索引两个版本，在快照装配侧（`AccessConfigCompiler`/`RouteSnapshotPublisher` 一侧）于任一变更时重算，请求期只做一次查表。
- limits 新增 `EdgeFunctionLimits`（挂进 `AccessConfigLimits` 并提升 `schema_version`）：索引 payload 上限、函数数上限、单函数脚本字节数、挂载条目数、编译产物总大小估算、每函数 report 次数、cidr directive 相关上限（§3.4），全部进 validator `--describe-config-limits` schema。

### 3.7 观测

- metrics（命名对齐 `access_server_*` 既有风格）：
  - `access_server_function_evaluations_total{function, project}`（项目 label 走 bounded 动态家族）
  - `access_server_function_sampled_out_total{function, project}`（桶区间外跳过，含 ratio 0 与被灰度取代的部分；与 evaluations 对比即实测命中率。版本灰度无需新指标：直接对比两个函数的 evaluations，比较错误率时按 evaluations 归一化）
  - `access_server_function_duration_seconds`（histogram，function label）
  - `access_server_function_errors_total{function, reason}`（Exception/Abort/Timeout/OversizeReport）
  - `access_server_function_events_total{function, event, label}`（§3.5）
  - `access_server_function_config_failures_total`（索引/脚本组装失败）
  - `access_server_functions_configured`（gauge，当前快照函数数）
- **访问日志 19 字段 Java 契约不动**。函数事件（含短路）写独立结构化日志行：requestId/traceId/project/host/function/event/label + 截断的目标片段。
- CAT：每次**实际执行**（采样命中的挂载；`sampled_out` 不记）在请求 message tree 里记一个子事务 `type=Access.EdgeFunction`、`name=<函数名>`，duration 由客户端在 complete 时取墙钟（含 `co_await` 挂起）。data 标签携带执行代价：
  - `phase=head|body`（同一函数两阶段各执行一次时产生两个事务）；
  - `alloc_bytes` = 执行窗口内 `GcHeapStats.allocated_bytes_total` 差分（毛分配量，主口径）；`heap_bytes` = `current_bytes` 差分（净留存，可为负——窗口内阈值回收可能清掉前序函数的垃圾）；窗口内有完成回收时附 `gc`/`gc_freed`。口径为 GC 请求字节数，不含 intern 桶、root 池与分配器开销；
  - 终态：成功（含短路响应）status=0；Exception/Abort status=ERROR 附 `reason`（与 errors_total 词表一致）及 abort 的 `detail`；协程取消时析构兜底 status=Fail 附 `reason=canceled`。
  CAT 根不可用（未配置客户端）时全程 no-op。事务在 `Access.Provider` 之前顺序完结，树序天然干净。

## 4. 控制面：WAF 编译器

### 4.1 规则模型（控制台内部模型，不进 wire）

```yaml
groups:
  - id: sqli # slug，≤60 字节，不可改名；发布为边缘函数 waf-sqli
    name: 'SQL 注入'
    mode: block # off/log/block；off = 不发布该函数
    priority: 100 # 即挂载 priority：组间顺序，也决定与手写函数的先后
    projects:
      include: ['*'] # glob，即 wire 挂载 include
      exempt: # glob + 必填原因，即 wire 挂载 exclude
        - project: loan-web
          reason: '内部报表导出含 SQL 片段, 2026-11-05 张三'
rules: # 平铺；每条规则只属于一个组，id 全策略唯一
  - id: sqli-0023 # 稳定 ID，进日志/metrics 标签
    name: 'union select 注入探测'
    group: sqli
    severity: high # critical/high/medium/low，仅元信息
    priority: 100 # 组内 if 链顺序
    action: block # log | block
    phase: request_head # v1 仅此枚举
    match: | # 表达式，与路由 condition 同语言同编译管线
      strings.match($req.uri, "(?i)union[+%20\\s]+select")
      || strings.match($query.q, "(?i)union\\s+select")
    scope: # 折叠进生成脚本
      methods: [GET, POST]
      paths: ['/api/*']
      source_cidrs: ['172.28.0.0/16'] # 生成器收集去重 → 脚本顶部 cidr directive 声明
projectModes:
  loan-admin: log # 覆盖该项目所在全部组的模式；off = 从这些组的挂载排除
```

**规则组（2026-11-07 修订，策略 schemaVersion 2）**：Project 作用域只在组上，规则不再带 Project。每个 mode 非 off 且含规则的组编译为一个边缘函数 `waf-<组 id>`，组的 `include/exempt` 直接成为 wire 挂载选择器，因此 native、rnacos 契约与 validator 均无改动。有效模式 = `projectModes[P] ?? group.mode`。

- 组数上限 32（函数总数 64 与手写函数共用）；exempt 与命中该组的 off 项目合计不得超过 selector 上限 256，保存时校验。
- `waf` 与 `waf-*` 为生成函数保留名；存量手写草稿可读取以便改名，但保存和创建 Release 都拒绝。
- 组内 block 后脚本 `return`；函数链在响应发出后短路，所以前面的组拦截后后面的组不执行，其 log 命中会少计。运行期异常只影响所在的组。
- 代价：不再有单规则豁免。规则误报时豁免整组、拆出单独的组，或用 `scope.paths` 收窄；起手规则包因此按误报族分为 6 个组，导入时全部为 off。
- 每多一组，请求多一次 `exec_async`、两次时钟读取和两次指标更新，实测约 0.3µs CPU（§5）；`function_evaluations_total` 上限 4096 个序列，组数 × 项目数超限会逐出并清零计数。
- schema 1 草稿读取时升级：规则进入 `default` 组，`defaultMode` 成为组模式（default off 时组的 include 改为已启用项目）；`exclusions` 折叠为 match 内的 `$ctx.project != …` 守卫，其审计原因丢失。回滚使用历史 wire payload，可回到单函数 `waf`，此时 `waf-*` 脚本成为孤儿（§8）。

脚本做不了的事由信封/生成器补：CIDR 判断经 `scope.source_cidrs` 语法糖编译为脚本顶部的 cidr directive 声明（多条规则共享的网段去重合并），规则体引用 `<name>.contains($conn.remote_addr)`；手写场景（P3）可直接写 directive，两者兼容。性能敏感的全量扫描由 `scope.methods/paths` 折叠为 `strings.hasPrefix`/`contains` 前置链（公共前缀合并，不逐规则跑正则）。需要完整脚本表达力的场景走既有 Script 路由类型，不往 WAF 规则里塞。

校验阶段对齐现有分层：字段语法（类型/长度/枚举/ID 唯一性）→ 交叉校验（规则引用的组存在、scope 字段与 CIDR 合法、挂载 exclude 不超限）→ 全环境校验（**生成产物必须通过 native validator 编译**，fail-closed，Release 准备之前）。

### 4.2 代码生成器（`server/`，关键路径组件）

- 输入：规则组 + 规则集 + 项目 mode 表；输出：每组一份函数脚本文本（含顶部 cidr directive 声明）+ 索引条目（exclude 含组豁免与命中该组的 mode=off 项目）。
- 生成策略：priority 升序 if 链；mode 查表在脚本头部；`block` 命中 = `ctx.report("waf.block", "<ruleId>")` + `resp.sendJson(403, …)` 默认 JSON 体 `{code:"WAF_BLOCKED", ruleId}`（实现修订：v1 方言不暴露 requestId/traceId 常量，二者不入体——traceId 经响应头 `hi-trace-id` 关联，ruleId 足以定位规则）；`log` 命中 = 仅 `ctx.report("waf.log", "<ruleId>")`。转义器实现要点（P2 落地）：非可打印 ASCII 与全部元字符统一走 `\xHH` 按字节转义，回避方言的八进制/`\a`/`\u{}` 转义死角，可证明字节级往返。
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
- 项目策略页：per-project mode 覆盖与规则组豁免（豁免带原因，可审计）。
- 发布预览：编译两个已存草稿（WAF 策略 + 手写函数）得到 planned 索引，与 rnacos live 索引**逐函数对比**（新增 / 不变 / 变更（脚本|挂载，含脚本 diff）/ 残留），全部脚本可编译才允许创建 Release。
- 边缘函数页：手写函数工作区（函数列表 + 脚本 CodeMirror 编辑 + 挂载编辑 + 执行阶段选择（P3 ①）+ 去抖逐函数原生编译预览（按 phase）+ If-Match 乐观并发保存），rnacos 实时快照折叠只读保留（§7 P3 ②，已实施；实时快照含 phase 展示）。

## 5. 性能预算

- 挂载项目每请求：1 次 `exec_async`（GcHeap 已存在，无新增分配；纯同步函数不挂起，成本同 `exec_sync`）+ 生成代码的前置字符串链；CIDR 判断为纯 C++ 区间匹配，无 VM 参与。
- 采样：每请求至多一次 crc32(客户端 IP 字节) + 整数比较（无分配）；命中的函数列表全部必跑（ratio 缺省/10000）时不抽签，零成本。`replaces` 的桶区间在快照组装时算好，请求期没有额外成本。
- 目标：典型 50 规则集函数求值 p99 < 100µs；`benchmark:native` 尚无边缘函数基准，这个目标还没有量化基线。
- 规则组实测（2026-11-07，compose 栈，单项目，WSL2，静态响应路由，起手规则包 9 条全部 log 且不命中，5 轮中位数）：access-server 每请求 CPU（cgroup `usage_usec`）无 WAF 32.4µs、1 个组 37.1µs、6 个组 38.6µs，即 WAF 约 +5µs，每多一组约 +0.3µs；三者的吞吐与延迟差异都在噪声内，该测试受压测客户端限制（约 2.9 万 RPS）。
- 未挂载项目零成本；mode=off 项目经挂载排除零成本；区间为空的函数（ratio=0 或被全量取代）只余一次比较。
- 热路径承诺：数据面侧短路判定（`response_header_sent()`）与索引查表无分配。

## 6. 安全与失败语义

- **fail-open**：函数 Exception/Abort → 放行 + `function_errors_total` 计数 + 日志（WAF 挂了不能打死网关）。严格模式为实例级配置项 `ACCESS_SERVER_EDGE_FUNCTION_FAIL_CLOSE=true`（P3 已实现）：函数在写出任何响应**之前**出错 → 整个请求以 503 `EDGE_FUNCTION_FAIL_CLOSE` 拒绝（错误仍计数+记日志，链路停止）；脚本已提交的响应不可撤回，已发送即生效。watcher 组装失败仍保持旧快照——fail-close 只约束执行期错误，不因配置面故障拒绝流量。
- 输入截断：进入脚本的每个字符串字段截取前 N KiB（默认 8KiB，limits 可调），防长 header/URI 拖慢求值。
- **VM 无 step budget**（`InterpreterVm` 无指令预算；`ScriptAbortReason::Timeout` 仅有枚举位）。v1 缓解：方言静态封堵（无函数定义/递归/`while`，唯一循环是 for-of over 有限容器）+ 表达式/脚本/产物上限 + 输入截断。根治手段是**上游建议**：InterpreterVm 增加可选 step budget/deadline；顺带建议上游更新 `docs/script-guide`（`strings.match/findAll` 在当前 pin 已注册，文档仍写"未注册"）。
- 正则：`strings.match` 为 RE2 线性实现 + 编译缓存 + WorkBudget，无回溯炸弹。CIDR 经 directive 编译期解析，畸形 CIDR 在发布期失败。
- 生成器安全：转义器单测 + 黄金 fixtures + validator fail-closed 三层兜底。
- 部署依赖与顺序：**fleet 需先升级到 pin ≥ 4a5f12b8**（`strings.match/findAll` 依赖；111/218/219 目前在旧 pin）。新二进制在无函数索引时零行为变化——先升二进制（ctl upgrade，窗口 <2ms），再发布首个函数索引 + 脚本，灰度顺序：单项目 log 模式 → 观察 → block；函数级另有 `ratio` 采样阶梯（§3.1）：小比例试跑 → 提比例 → 100%（转缺省/必跑）。 2026-11-07 的 wire 变更（拍平 + `replaces`）非向后兼容：codec 拒绝未知字段，旧二进制收到新索引会保留旧快照、灰度不生效，反向同理，因此先升全部实例与 validator，再发布新索引。

## 7. 阶段计划

| 阶段          | 内容                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P0            | fleet 升级至 pin ≥ 4a5f12b8；上游建议（step budget、script-guide 文档）                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| P1 native     | `EdgeFunctionLimits` + 索引/脚本 codec + validator 扩展 → `EdgeFunctionsWatcher`（hash 校验 + 内容寻址编译缓存 + 全或无组装）+ 联合索引 → 受限 Library（含 cidr directive 扩展）+ 编译 → 管线钩子 + 短路 → `ctx.report` + metrics + 事件日志 → CTest 覆盖（含黄金 fixture 的 native 侧编译断言、cidr directive 编译期失败用例）                                                                                                                                                                                               |
| P2 server+web | `modules/waf` + 转义器/生成器 + 黄金 fixtures + 发布资源条目（先脚本后索引）+ 激活证据；WAF 页 + 项目策略页 + 发布预览；边缘函数只读页                                                                                                                                                                                                                                                                                                                                                                                        |
| P3            | body phase（缓冲-重放、per-route opt-in `edge_function_body`、content-type 白名单、大小上限`maxBodyPhaseBytes`——219 大文件分块上传的前车之鉴，绝不全量缓冲；溢出保留有界前缀拼活尾部）**① 已实施**（native `52682b3` + server `3d2a048` + web）；手写边缘函数开放（先扛住 step budget / 基数上限硬约束）**② 已实施**；fail-close 选项**⑤ 已实施**（`908a1d2`）；起手规则包（SQLi/XSS/路径穿越/扫描器 UA/畸形 URI，纯数据）**③ 已实施**（`ca691d8`）；控制台命中统计页（server 集成 Prometheus 查询）**④ 已实施**（`a093da2`） |

## 8. 开放项

- **孤儿脚本 Data ID 清理**（发布侧）：函数从索引移除后其脚本 Data ID 仍留在 rnacos（数据面按索引组装、直接忽略孤儿，无正确性影响，但字节残留且快照上限按索引内函数计）。console 的 NacosClient 只有 read/write，`release_resources` 的 remove 操作在发布 worker 中未实现（worker 只认 `required_resource = TRUE`）。需要：nacos delete 集成 + worker remove 路径 + 移除资源与失败语义设计。

- ~~**上游备忘失效缺陷**：`ScriptExchangeCtx::set_path_vars()` 不清除已备忘的 `$path` 常量~~ **已解决**（上游 `625f0b46`，随本设计实施一同 pin）：`set_path_vars()` 现重置整个常量备忘（单条摘除会断线性探测链），各候选条件读到自己的捕获；`AccessRequestHandlerTest.PathVarConditionSeesEachCandidatesOwnCapture` 已解封守护。
- **body 阶段 v2 候选**（P3 ① 后仍开放）：content-type 白名单为代码固定 v1（console/数据面均不可配置——可配置清单是配置面攻击面，先固定观察）；`multipart/form-data`、`application/octet-stream` 等是否纳入待真实规则需求；脚本在代理路径上改写响应头（`resp.setHeader` 未 send 即丢弃，见 §3.3 方言脚枪）需要显式契约；`$req.body` 目前整体原样视图（无解析后的 JSON 常量，规则用 `strings.match` 匹配原始字节）。
- `access_server_function_duration_seconds` 按整数微秒截断（`duration_cast<microseconds>`），1µs 以内的执行记为 0。规则组把脚本拆小后更容易低估，判断函数开销要看 CPU，不要看这个直方图；修复需改 native 记录精度。
- ~~CAT 事务标记形态~~ **已定案并实施**（§3.7）：每次实际执行记 `Access.EdgeFunction` 子事务（含代价标签），短路不是独立形态——它只是该事务的 status=0 终态。
- 函数能否读 trace context（`$context` 已含 trace cluster；开放给 `$ctx` 的范围待实现时定）。
- 响应阶段函数（`phase: response`）与请求改写（`phase: request_rewrite`）的契约扩展——远期，先留枚举位不实现。
- 限流作为第二个编译目标的可行性（依赖每 worker 状态与时间窗原语，数据面需新增受控 API）。
