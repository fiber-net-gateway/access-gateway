# Method 与 JavaScript Route 详细设计

本文实现 [Method 与 JavaScript Route 需求](method-and-js-routes-requirements.md)。

## 1. 设计原则

1. rnacos 边界仍是单个 Project JSON，不引入第二个 Route Data ID；
2. Route 顺序、Host/Path matcher、不可变快照和请求 pinning 保持不变；
3. method 是轻量前置谓词，不把 method 拼进 path，也不在请求期分配字符串；
4. JavaScript 在配置更新期编译，在请求期只执行编译产物；
5. Console 不执行脚本，Native Validator 与 access-server 使用同一 codec/compiler；
6. 任何新字段错误都拒绝整个候选，不能发布部分 YAML/JS Route。

## 2. Console 持久化模型

schema 升级为 v5，顶层 `kind` 保留 `project_routes_yaml`，避免改变现有 API/数据库领域判别；
是否为混合 Route 由条目级 `format` 判别：

```ts
type RouteItemModel =
  | { id: string; format: 'yaml'; source: string }
  | { id: string; format: 'js'; source: string; path: string; method?: string }

interface ProjectRoutesModel {
  schemaVersion: 5
  kind: 'project_routes_yaml'
  networkPolicy: ProjectNetworkPolicy
  routes: readonly RouteItemModel[]
}
```

选择保留顶层 kind 是兼容性决策；它是已有配置领域的稳定标识，不再解释为“数组里只能有 YAML”。

### 2.1 升级

- schema v4：每个 `{id, source}` 变为 `{id, format:'yaml', source}`；
- v1-v3：沿现有升级链先归一为 v4，再补 `format`；
- UUID、顺序、YAML 字节内容和网络/HTTPS 默认值不变；
- 归一化发生在解密读取边界，不回写或改写历史 revision/release。

### 2.2 大小限制

- 单条 source 继续限制为 1 MiB；
- Project 所有 source 总计继续限制为 4 MiB；
- JS `path` 1-2048 字符，`method` 1-64 个 ASCII 字节；
- Route 数量继续限制为 5000。

## 3. 服务端编译器

`compileProjectRoutes()` 按条目 format 分派：

```text
yaml -> 安全 YAML parse -> 字段/shape 校验 -> wire route object
js   -> 外置 path/method 校验 -> {path, method?, type:'SCRIPT', script:source}
                                     |
                                     +-- Project allows 注入（若启用）
ordered wire routes -> canonical JSON -> SHA-256 -> Native Validator
```

YAML allowlist 增加 `method`。YAML 仍只允许 `PROXY`/`RESPONSE`，从而保证 JavaScript 只能通过
显式 JS 条目产生，避免一份 YAML 同时携带脚本和外置元数据。

method token 使用 RFC 9110 token 字符集合：

```text
! # $ % & ' * + - . ^ _ ` | ~ DIGIT ALPHA
```

不 trim、不折叠大小写。空、空白、分隔符或非 ASCII 字节均报 `INVALID_ROUTE_METHOD`。

JS source 空字符串报 `EMPTY_ROUTE_SCRIPT`；path 复用 native path pattern 的最终校验，本地先做非空和
长度校验。脚本语法不在 Node 中解析，交给 Native Validator，防止出现两个脚本语义来源。

该增量当时把编译器 revision 更新为 `project-routes-mixed-v5-method-script`；后续功能继续递增。

## 4. Native wire 与 codec

`RouteConfig` 增加：

```cpp
std::optional<std::string> method;
std::optional<std::string> script;
```

`RouteType` 增加 `Script`，codec 接受 `"SCRIPT"`。沿用现有 Java 风格 scalar coercion：method/script
通过 nullable string decoder 读取；未知 JSON 字段继续按当前 codec 行为忽略。语义编译阶段负责：

- method 非空且为合法 token；
- SCRIPT 必须有非空 script；
- SCRIPT 拒绝静态 PROXY/RESPONSE 专属字段；
- PROXY/RESPONSE 拒绝非空 script。

为了不误伤直接写 rnacos 的旧 payload，`method:null` 与字段缺失相同；未出现 `script` 时旧 type
默认仍为 PROXY。

## 5. 快照编译和冲突检测

`CompiledRoute` 增加 optional method 和 script program。Route matcher 的 key 规则扩展为：

```text
无 method 且无 condition: path
有 method 或 condition:  path + '@' + CRC32C(canonical predicate signature)
```

signature 带字段名和长度边界，避免 `method`/`condition` 简单连接歧义。key 只在构建期分配。

`PendingRouteCompile` 增加 `has_predicate`。RouteDefiner 仅在同 path 节点此前已有
`has_predicate == false` Route 时报告 dead route。相同 method/condition 产生相同 key，由
RouteDefiner 的节点级 key 集合拒绝；不同 method 可共存。

脚本编译步骤：

1. matcher 解析 path 并收集 path variable name；
2. `RouteScriptExtension::CompileScope` 绑定项目级 ConstPackage builder；
3. 使用同一 process-lifetime `StdLibrary` 编译 source；
4. 允许赋值和 async，禁用 HTTP upstream directive；
5. 编译错误映射到 `routes[i].script`；
6. 所有 Route 成功后一次 build ConstPackage 并发布快照。

## 6. 请求匹配

`RouteMatchContext` 持有只读 `exchange.method_view()`：

1. Path matcher 给出同节点候选；
2. 若 route.method 存在且与请求 method 不完全相等，立即跳过；
3. 绑定 path constants；
4. 若 condition 存在则执行；
5. 第一个全部通过的 Route 胜出。

method 比较不分配、不哈希；失败候选不绑定/清理常量。未命中保持现有
`URL_NOT_MATCHED`/404 行为，不计算 405。

## 7. JavaScript 执行

`AccessScriptRuntime` 扩展为三组 adapter：

- `compile_expression`：现有 condition/template，同步、禁止赋值；
- `compile_route_script`：完整脚本，允许 async；
- `execute_route_script`：使用 telemetry 已持有的 request-scoped `ScriptExchangeCtx`/heap。

注册 Fiber 公共 `register_http_functions_to_lib()`，提供 req/resp；不在 access-server 内复制
Fiber 实现。执行前常量已由项目快照准备，path captures 已绑定。Host/HTTPS/entry/CIDR/body-length
前置策略通过后才执行脚本。

响应规则与 Fiber `script_file` 保持一致：

| 结果            | 未显式发送响应时的行为      |
| --------------- | --------------------------- |
| Value           | 200 JSON                    |
| Void            | 204 empty                   |
| Exception/Abort | 返回 `SCRIPT_EXECUTION` 500 |

如果 `resp.send*` 已提交响应，后续结果不触发第二次写入。宿主在执行前把 HSTS 和 trace header
加入脚本响应上下文；脚本可通过 Fiber 的响应状态机设置业务 header，协议 framing 仍由
`HttpExchange` 生成。

请求 body 使用全局 body limit。已知 Content-Length 在脚本执行前检查；无 body 可直接执行；当前
Fiber 脚本读取接口无法让宿主对未知长度流累计计数，因此 chunked/stream body 在进入脚本前以 413
拒绝。这样不会为 JS Route 引入无界内存读取，未来应通过上游有界读取 API 再放宽。

本期不为 `ScriptExchangeCtx` 注入 `HttpScriptServices`，因此编译期关闭 `directive ... = http ...`。
未来接入时应在 Fiber 上游提供可复用 access-server pool/service bridge，而不是复制 lite-nginx 私有实现。

## 8. Console UI

- 工具栏保留 `+ RESPONSE`、`+ PROXY`（均创建 YAML），新增 `+ JS`；
- 卡片显示 `YAML/JS`、执行类型、path、method 或 `ALL METHODS`；
- YAML 使用现有 CodeMirror YAML mode，并增加 method completion；
- JS 使用独立 CodeMirror JavaScript mode；外置 path 为必填输入，method 为可选文本输入；
- format 在创建后不可就地切换，避免静默丢失 YAML 字段；用户可新建目标格式后复制正文；
- 本地问题阻止保存，Native 问题按 routeId/field 定位到 source/path/method；
- 历史预览显示 format 和 JS 外置匹配元数据。

## 9. 安全、性能和生命周期

- 脚本只在 Native 进程执行，不在浏览器、Node 或 validator 响应中回显执行结果；
- validator 错误仅含 route index、field 和有界编译消息，不含 source；
- 编译产物和 ConstPackage 属于不可变 Project snapshot，请求通过 shared snapshot pin 保活；
- request heap 属于请求，异步脚本必须在 owner EventLoop 完成或随 exchange 取消；
- method 热路径仅一次 optional 检查和等长字节比较；
- 不新增 method/project/path metrics label；
- 配置编译失败、同版本更新和 shutdown 顺序沿用现有 RouteConfigStore/Watcher 语义。

## 10. 测试矩阵

### Native

- Codec：method、SCRIPT、null/coercion；
- Snapshot：token、SCRIPT 组合、脚本编译、same-path method、fallback/dead route；
- Handler：GET/POST 分流、method+condition、JS sync/async response、path constant、异常；
- Store/Watcher/Validator：失败保留旧快照、字段路径和 summary。

### Console

- Model：v1-v4 升级、mixed union、限制；
- Compiler：YAML method、JS wire、混排顺序、method/source/path 错误、网络策略注入；
- API：schema v5 保存/恢复/校验；
- UI：新增/编辑/复制 JS、外置字段、错误阻止保存、历史预览。

### 资格声明

上述测试证明仓库内契约与实现一致，但生产脚本 corpus differential 和最终 cutover gate 仍未完成，
不能据此宣称生产兼容切换已经完成。
