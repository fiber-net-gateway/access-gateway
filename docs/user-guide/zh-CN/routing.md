# Access Gateway 路由规则与详细用法

本文面向在 Console 中配置 Project 路由的开发者和运维人员，说明当前仓库实现的 Host、Path、
Method、条件、CIDR、`RESPONSE`、`PROXY` 和发布语义。配置的最终判定者是
`native/access-server` 的 wire codec、路由编译器和 Native Validator，而不是浏览器中的语法提示。

> 资格说明：本文以仓库自有的 C++ data plane 和固定 Fiber revision
> `0fda7764bf94944aca4b674ab5ab311184703118` 为准。生产脚本 corpus 差分和最终切流门禁仍未全部完成；
> 本文不代表已经具备生产切流资格。

## 1. 核心模型

一个入站请求只由 `access-server` 接收和代理。Console 负责编辑、校验、版本和发布，不转发普通网关流量。

一次请求大致按以下顺序处理：

1. 固定当前不可变配置快照，保证请求处理中途不会切换版本；
2. 规范化 `Host`，在全局 Host 树中选择 Project；
3. 执行 Host 级 `X-Entry` 和 HTTPS 策略；
4. 按 Path 结构选择候选，再依次判断可选 `method` 和 `condition`；
5. 对命中的 Route 检查请求体大小和 `X-Real-Ip` CIDR；
6. 执行 `RESPONSE`、`PROXY` 或 `SCRIPT`；
7. 写入有界 metrics、trace 和结构化 access log。

路由列表有顺序，但“顺序优先”只适用于同一个 Path matcher 节点的候选。不同结构之间先按
静态段、参数段、尾部通配段的 matcher 优先级选择。

## 2. 配置与生命周期

### 2.1 三种状态不能混为一谈

| 状态               | 含义                                        | 能否证明实例正在使用 |
| ------------------ | ------------------------------------------- | -------------------- |
| Draft / 配置版本   | 已保存到 Console 数据库的不可变内容         | 不能                 |
| Published          | 内容已经写入并回读 rnacos                   | 不能                 |
| Active on instance | 特定 access-server 实例提供的类型化激活证据 | 能                   |

rnacos 写入成功或 readback 一致只证明“已发布”，不证明任何实例已经激活。当前没有实例级证据时，
Console 必须显示“未知”，不能把 Published 渲染成 Active。

### 2.2 Console 编辑模型

一个 Project 的 Route 列表可以混合两种编辑格式：

- YAML Route：正文是一条 YAML mapping，`path`、`method` 和执行字段都写在正文中；只允许
  `PROXY` 或 `RESPONSE`。
- JavaScript Route：正文只写脚本，`path` 和可选 `method` 在编辑器外单独填写；发布时编译为
  wire `type: SCRIPT`。

每个编辑器只包含一条 Route，不要在一个 YAML 编辑器中写 `routes:` 数组。拖动或键盘排序会改变
候选顺序；保存会创建新的不可变配置版本，不改写历史版本。

Console 使用 YAML 1.2 core schema，并额外要求：

- 根节点必须是 mapping；
- key 必须唯一；
- 禁止 anchor、alias 和显式 tag；
- 值只能是 JSON 安全的字符串、布尔值、数组、对象、`null` 和安全整数；
- 未知字段直接拒绝，而不是依赖 native 忽略；
- 单条 source 最大 1 MiB，一个 Project 的所有 source 合计最大 4 MiB，最多 5000 条 Route。

## 3. Host 与 Project 选择

### 3.1 Host 规范化

Host 匹配不区分 ASCII 大小写。匹配前会去除端口和一个末尾的点；带方括号的 IPv6 literal 会保留
地址本身并去除其后的端口。空 Host、连续点、斜杠、控制字符等非法值不会进入 matcher。

Host pattern 支持：

| Pattern         | 示例              | 匹配                                                              |
| --------------- | ----------------- | ----------------------------------------------------------------- |
| Exact           | `api.example.com` | 只匹配该 Host                                                     |
| Global wildcard | `*`               | 匹配任意合法 Host                                                 |
| Suffix wildcard | `*.example.com`   | 匹配 `a.example.com` 和 `a.b.example.com`，不匹配裸 `example.com` |

不同 Project 不能声明规范化后重复的 Host pattern。Console 当前通常从 Project domain 生成 exact Host；
直接维护 wire 配置时仍必须遵守上述全局冲突规则。

Host 未命中返回 HTTP 404，错误名为 `ROUTER_NOT_FOUND`。只有 Host 命中后，才会继续选择 Path Route。

### 3.2 HTTPS 和入口策略

Host 的 HTTPS 策略可为不强制，或使用 301、302、307、308 重定向。请求 scheme 或
`X-Forwarded-Proto` 大小写不敏感地等于 `https` 时视为 HTTPS；否则返回：

```text
Location: https://<effective-host><original-uri>
Strict-Transport-Security: max-age=31536000
```

Host 命中后，access-server 会为后续响应准备 HSTS header。Route 的有效
`response_headers` 可以覆盖它。若配置了网络入口 mask，`X-Entry` 必须匹配 `vdi`、`desktop` 或
`internet` 中允许的入口，否则返回 403 `ENTRY_ERROR`。

## 4. Path pattern

Path pattern 只接受 ASCII 字节，并按 `/` 分段：

| 形式       | 示例            | 含义                                  | 可在脚本/模板中读取 |
| ---------- | --------------- | ------------------------------------- | ------------------- |
| 静态段     | `/users/me`     | 精确段匹配                            | 无                  |
| 参数段     | `/users/:id`    | 捕获一个 segment；尾部 `/` 可产生空值 | `$path.id`          |
| 尾部通配段 | `/assets/*rest` | 捕获剩余 path，必须是最后一段         | `$path.rest`        |

示例：`/tenants/:tenant/files/*rest` 匹配
`/tenants/acme/files/a/b.txt`，得到 `tenant = "acme"`、`rest = "a/b.txt"`。

规则和限制：

- `*name` 必须是最后一个 segment；`/assets/*rest/meta` 会在候选快照编译时失败；
- 同一 pattern 不能重复变量名，例如 `/:id/children/:id` 无效；
- 静态分支优先于参数分支，参数分支优先于 wildcard 分支；
- Path 大小写敏感；
- matcher 按 Java 兼容行为折叠重复 `/`，并保留特殊的尾部斜杠行为；例如只有
  `/a/b/:value` 时，`/a/b/` 会命中且 `value` 为空字符串。不要用重复或尾部 `/` 区分业务路由，
  应发布规范化 pattern；
- query string 不参与 Path 匹配；使用 `$query.*` 或 `req.getQuery()` 读取 query；
- `$path.name` 必须引用当前 pattern 已声明的变量，否则 condition、template 或脚本编译失败。

只有 method 不匹配时不会返回 405；所有 Path 候选都失败时统一返回 404 `URL_NOT_MATCHED`，也不会自动
添加 `Allow` header。

## 5. Method、condition 与顺序

### 5.1 Method

`method` 可省略或设为 `null`，表示匹配所有 method。配置后必须是 1–64 字节的 HTTP token，合法字符为：

```text
! # $ % & ' * + - . ^ _ ` | ~ DIGIT ALPHA
```

匹配按原始字节精确比较且大小写敏感。推荐写标准大写形式，例如 `GET`、`POST`；`get` 只会匹配实际的
小写 `get`，不会自动规范化。

### 5.2 Condition

YAML Route 可增加同步 `condition` 表达式：

```yaml
path: /reports/:id
method: GET
type: PROXY
condition: '$query.preview == "1" && $header.x_role == "reviewer"'
service: report-service/stable
```

`method` 和 `condition` 是 AND：先比较 method，匹配后才绑定 `$path` 并执行 condition。condition 必须是
一个表达式，不写 `return`；异步函数（例如 `req.readJson()`）会导致配置编译失败。表达式返回 falsey、
抛出未捕获异常或执行失败时，该候选被视为“不匹配”，继续尝试同节点的后续候选。

### 5.3 同一路径的候选顺序

下面的顺序有效：

1. `GET /items/:id`，带更具体 condition；
2. `GET /items/:id`，无 condition；
3. `POST /items/:id`；
4. all-method `/items/:id` fallback。

同一路径可以有不同 method 或不同 condition。完全相同的 predicate 会被拒绝。一个无 method 且无
condition 的 Route 是该节点的无条件终点；它后面再放同节点 Route 会形成 dead route，整个候选快照
都会被拒绝。

因此 fallback 必须放在相同 Path 候选的最后。不要假设把静态、参数和 wildcard Route 拖动换序就能覆盖
matcher 的结构优先级。

## 6. YAML Route 字段参考

| 字段                   | 适用类型       | 规则与默认值                                               |
| ---------------------- | -------------- | ---------------------------------------------------------- |
| `path`                 | 全部           | 必填、非空、ASCII Path pattern                             |
| `method`               | 全部           | 可选；缺失/`null` 匹配全部，配置后精确匹配                 |
| `type`                 | 全部           | YAML 中必须为 `RESPONSE` 或 `PROXY`                        |
| `condition`            | YAML           | 可选同步表达式                                             |
| `service`              | PROXY          | 命名服务；可写 `service/cluster`                           |
| `cluster`              | PROXY          | 显式 cluster，覆盖 `service` suffix                        |
| `addresses`            | PROXY          | 无 `service` 时使用的静态 HTTP(S) authority 列表           |
| `proxy_headers`        | PROXY          | 发往 upstream 的 header 模板 mapping                       |
| `response_headers`     | RESPONSE/PROXY | 发往 client 的 header 模板 mapping                         |
| `context`              | PROXY          | trace/CAT context 模板 mapping                             |
| `rewrite`              | PROXY          | upstream path 模板；原 query 会重新拼接                    |
| `status`               | RESPONSE       | 必填；native 接受 100–999，实际应使用标准 HTTP status      |
| `body`                 | RESPONSE       | 可选；缺失表示空 body                                      |
| `timeout`              | PROXY          | response header timeout；默认 60000 ms，配置值至少 5 ms    |
| `max_client_body_size` | RESPONSE/PROXY | Route 请求体限制；缺失或数值 0 使用 server 默认值          |
| `max_proxy_body_size`  | PROXY          | 非零时覆盖 upstream response body 限制                     |
| `websocket_timeout`    | PROXY          | 大于 0 时允许 WebSocket tunnel，并作为隧道读写 timeout     |
| `flush`                | PROXY          | `true` 开启低延迟 body flush，并写 `X-Accel-Buffering: no` |
| `allows`               | 全部           | CIDR allow/deny sequence；`!` 前缀表示 deny                |

时间值接受整数毫秒，或不区分单位大小写的字符串：`500`、`500ms`、`5s`。数据大小接受整数 byte，
或二进制单位字符串，例如 `64k`、`4m`、`1g`。新配置应使用非负、含义明确的值；不要依赖 Java
兼容层保留的整数溢出和负值行为。

Header mapping、`context` mapping 的 value 必须是 scalar 或 `null`，Console 会在发布前确定性编译为
wire scalar。敏感 header value、Authorization、cookie 和 body 不应出现在文档、变更说明或日志中。

## 7. RESPONSE Route

### 7.1 文本响应

```yaml
path: /healthz
method: GET
type: RESPONSE
status: 200
body:
    type: TEXT
    content: ok
response_headers:
    Content-Type: text/plain; charset=utf-8
    Cache-Control: no-store
```

`TEXT` 的 `content` 必须非空，按 UTF-8 字节发送。

### 7.2 Base64 响应

```yaml
path: /pixel.gif
method: GET
type: RESPONSE
status: 200
body:
    type: BASE64
    content: R0lGODlhAQABAAAAACw=
response_headers:
    Content-Type: image/gif
```

`BASE64` 在配置编译期解码；非法字符或 padding 会拒绝候选快照，不会等到请求时才失败。

### 7.3 Template 响应

```yaml
path: /hello/:name
method: GET
type: RESPONSE
status: 200
body:
    type: TEMPLATE
    content: '{"message":"hello ${$path.name}"}'
response_headers:
    Content-Type: application/json; charset=utf-8
    X-Request-Method: "${$req.method}"
```

Template 是文本拼接，不会自动进行 JSON、HTML 或 URL 转义。把不可信输入插进 JSON/HTML 时必须在输出
边界正确编码；复杂 JSON 更适合使用 JavaScript Route 的 `resp.sendJson()`。

Template 中的 `${expression}` 在配置加载阶段编译，在请求阶段同步求值。支持 `\\`、`\$`、`\{`、
`\}` 字面转义。结果为 `null`、`undefined`、object 或 array 时产生空文本；scalar 转为兼容文本。

RESPONSE 会先排空请求 body，再原子准备所有 route response header 和 body。任一模板或 header 校验
失败时不会提交部分 Route 输出，而会进入稳定错误处理。

以下 hop-by-hop/framing header 不能由 Route response header 覆盖：

```text
Connection, Content-Length, Proxy-Connection, Keep-Alive,
Proxy-Authenticate, Proxy-Authorization, TE, Trailer,
Transfer-Encoding, Upgrade
```

## 8. PROXY Route

### 8.1 NamingService upstream

```yaml
path: /api/orders/:id
method: GET
type: PROXY
service: orders/stable
timeout: 30s
rewrite: "/internal/orders/${$path.id}"
proxy_headers:
    X-Tenant: "${$header.x_tenant}"
response_headers:
    Cache-Control: no-store
```

`service: orders/stable` 把服务名解析为 `orders`、默认 cluster 解析为 `stable`。单独配置非空 `cluster`
会覆盖 suffix。NamingService 只选择 enabled、healthy 且 weight 为正的实例；没有可用实例时返回稳定的
503 错误，而不是把请求交给 Console。

### 8.2 静态 upstream

```yaml
path: /legacy/*rest
type: PROXY
addresses:
    - http://127.0.0.1:8080
    - https://legacy.internal:8443
rewrite: "/${$path.rest}"
timeout: 20s
```

`service` 非空时优先使用 NamingService，`addresses` 不充当它的失败 fallback。希望使用静态地址时应
省略 `service`。未写 scheme 时，端口 443 推导为 HTTPS，其他端口推导为 HTTP；建议始终显式写
`http://` 或 `https://`，避免配置含义依赖端口。

### 8.3 请求与响应转发

- 原始 method 保留；
- 没有 `rewrite` 时保留 raw URI 和原始 percent-encoding；
- `rewrite` 只生成 path，结果为空时使用 `/`，原 query 随后拼回；
- downstream request body 流式转发，不在 Console/control plane 中落地；
- `proxy_headers` 先覆盖或抑制同名入站 header；空模板值不写该 header；
- 固定 hop-by-hop、framing 和 inbound `Host` 不直接透传；upstream `Host` 由目标 authority 生成；
- `x-ploto-source-app` 最终由 access-server 强制设置为 `<project>.unifiedAccess`；
- upstream status 原样返回；response hop-by-hop header 被过滤；
- `response_headers` 可覆盖普通 upstream header；显式覆盖 `Location`/`Refresh` 时不再自动改写；
- upstream body 受 `max_proxy_body_size` 或 server 默认限制。

普通连接失败只会在 upstream request header 尚未开始发送时安全重选，最多尝试 4 个选择（含首选）。
一旦 header 已开始发送便不重试，避免重复提交非幂等请求。

### 8.4 Flush 和流式响应

`flush: true` 用于 SSE、流式 JSON 和其他低延迟分块响应。它减少 access-server 本地响应 body 聚合，并
添加 `X-Accel-Buffering: no`。它不会自动关闭客户端和 access-server 之外的 CDN、Ingress、浏览器或
协议栈缓冲；端到端仍需单独配置和验证。

### 8.5 WebSocket

```yaml
path: /socket
type: PROXY
service: realtime/stable
websocket_timeout: 300s
```

只有 `websocket_timeout > 0`，且入站 HTTP/1.1 `Upgrade` 大小写不敏感等于 `websocket`、
`Connection` 大小写不敏感精确等于 `upgrade` 时才进入 Java 兼容 WebSocket 路径。逗号 token 列表不会
命中这项遗留判断。101 握手成功后切换双向 tunnel，timeout 用于隧道读写。

## 9. CIDR 与请求体策略

### 9.1 Route CIDR

```yaml
path: /admin/*rest
type: PROXY
service: admin/stable
allows:
    - 10.0.0.0/8
    - 2001:db8::/32
    - "!10.20.0.0/16"
```

无 `!` 的项组成 allow 集合，有 `!` 的项组成 deny 集合：

1. allow 集合非空且来源不命中任一 allow 时拒绝；
2. 命中任一 deny 时拒绝；
3. deny 优先于 allow 的最终结果。

来源取自 `X-Real-Ip`。为保持 Java 行为，该 header 缺失或无法解析时会跳过 CIDR 检查，因此可信边界
必须保证只有受信任的前置代理能写入/覆盖 `X-Real-Ip`。被拒绝返回 403 `NOT_ALLOW_IP`。

当 Console 的 Network Policy 选择 Project 级策略时，编译器会把统一 allow/deny 列表注入每条 wire
Route；此时 Route YAML 不应再声明 `allows`，否则会报策略冲突。

### 9.2 请求体大小

默认 request body limit 由 server 启动配置决定。Route 的 `max_client_body_size` 非零时覆盖它；超限
返回 413 `REQ_BODY_TOO_LARGE`。PROXY 对 Content-Length 和流式 body 都累计限制。

JavaScript Route 当前只能接受：

- 无 body；或
- 有合法 Content-Length，且长度不超过全局 request body limit。

未知长度的 chunked/stream body 会在执行脚本前 fail closed 为 413。JavaScript Route 的更多限制见
“脚本路由用法”。

## 10. Template 和 condition 可用变量

下面的常量在配置编译时解析、请求时取值：

```text
$path.<route-variable>
$query.<query-name>
$header.<normalized-header-name>
$cookie.<normalized-cookie-name>
$context.<context-name>

$req.uri        $req.method      $req.path       $req.query
$conn.remote_addr  $conn.remote_port  $conn.http_version  $conn.scheme  $conn.tls
```

`$header`、`$cookie` 和 `$context` 名称会执行 ASCII 小写折叠，并把 `-` 与 `_` 归一化。例如
`$header.x_forwarded_for` 可读取 `X-Forwarded-For`。含空格、点号等不能写成标识符的 query/cookie key，
使用 `req.getQuery("...")` 或 `req.getCookie("...")`。

condition/template 只开放同步标准库和同步 request metadata 函数；`resp.*` 不可用，读取 body 的异步
`req.*` 会被编译器拒绝。完整语法和函数列表见“脚本语法、标准库与 HTTP API 参考”。

## 11. rnacos wire 契约

默认数据位置不能随意改变：

| 用途            | Data ID                                | Group           | 内容                      |
| --------------- | -------------------------------------- | --------------- | ------------------------- |
| Project 列表    | `ploto.unified-access.projects`        | `ACCESS-SERVER` | 分号分隔字符串，不是 JSON |
| Project Route   | `ploto.unified-access.route.<project>` | `ACCESS-SERVER` | Project JSON              |
| Production 灰度 | `ploto.unified-access.gray-match`      | `DEFAULT_GROUP` | Gray JSON                 |

Project JSON 的简化结构：

```json
{
    "version": 42,
    "host": {
        "api.example.com": {
            "https": "S_NOT_MUST"
        }
    },
    "routes": [
        {
            "path": "/healthz",
            "method": "GET",
            "type": "RESPONSE",
            "status": 200,
            "body": { "type": "TEXT", "content": "ok" }
        }
    ]
}
```

不要直接把 Console 内部 normalized model 当作 rnacos payload。发布器必须确定性编译成上述兼容格式；
特别是 Project 列表始终是分号字符串。

## 12. 更新、失败和回滚语义

- 同一 Project 的候选 `version` 与当前版本相同：忽略，不重新激活；
- 非空候选先完整 JSON decode、Path 构建、CIDR/模板/脚本编译和 upstream 准备，再原子发布；
- 任一步失败：保留上一成功快照，请求不能看到半成品；
- Project route 内容为空：保持当前配置，不等价于删除；
- `host` 缺失或为空：卸载该 Project 的 Host/Route；
- Project 从项目列表删除：取消订阅并卸载，而不是“更新失败”；
- 回滚：从历史内容创建一个新 Release，不改写历史 Release；
- 每个 rnacos resource 的写入、readback、失败和后续实例证据都应独立记录，因为 rnacos 不提供跨资源事务。

## 13. 常见错误与排查

| HTTP / 错误名              | 常见原因                                           | 首要检查                                          |
| -------------------------- | -------------------------------------------------- | ------------------------------------------------- |
| 404 `ROUTER_NOT_FOUND`     | Host 非法、未配置或未命中                          | 实际 `Host`、端口、Project domain、wildcard       |
| 404 `URL_NOT_MATCHED`      | Path/method/condition 全部未命中                   | matcher 结构、大小写、候选顺序、condition 值      |
| 403 `ENTRY_ERROR`          | `X-Entry` 不在 Host 策略中                         | 可信前置代理写入值和网络策略                      |
| 403 `NOT_ALLOW_IP`         | `X-Real-Ip` 被 CIDR 拒绝                           | header 来源、allow/deny 交集、IPv4/IPv6 prefix    |
| 413 `REQ_BODY_TOO_LARGE`   | Route/global body limit，或 SCRIPT 的未知长度 body | Content-Length、传输编码、限制配置                |
| 500 template/script error  | 表达式运行失败或脚本未处理异常                     | Native Validator 字段路径、函数参数、`$path` 名称 |
| 503 no hosts/circuit break | 无健康 NamingService 实例或实例不可选              | service、cluster、实例 enabled/healthy/weight     |

排查配置更新时，先区分 Draft、Published 和 instance Active，再查看 Native Validator 的稳定错误 code 和
field path。不要把旧快照仍在服务误判为“坏配置已经生效”，也不要把 rnacos readback 成功误判为实例
激活。

## 14. 推荐实践

1. 从最具体的 method/condition 候选排到 all-method fallback；
2. 静态、参数和 wildcard pattern 各自保持清晰职责，不依赖难以直观看出的遮蔽；
3. 对公开 API 显式设置 method、body limit、timeout 和可信来源 CIDR；
4. 静态地址显式写 scheme 和 port；NamingService route 明确 service/cluster；
5. 简单常量响应使用 RESPONSE，普通流量转发使用 PROXY，需要请求期计算和 JSON 组装时才使用 SCRIPT；
6. condition 和 template 保持短小、同步、无副作用；复杂逻辑移到脚本路由或 upstream 服务；
7. 不在 header 模板、脚本错误、审计事件或文档示例中暴露凭据、cookie、Authorization 或业务 body；
8. 发布前使用 Native Validator；发布后等待真实实例证据，证据缺失时保持“未知”；
9. 兼容性结论必须同时包含 native/Console 测试和记录在案的 differential/cutover gate，不能只凭单元测试。
