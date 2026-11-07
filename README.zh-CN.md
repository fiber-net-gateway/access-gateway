# Access Gateway

[English](README.md) | 简体中文

Access Gateway 是一个自托管的 HTTP 网关，由两部分组成：

- **`access-server`**：基于 [Fiber](https://github.com/fiber-net-gateway/fiber-gateway-cpp) 框架的 C++23 数据面，负责接收客户端流量、选择路由，并响应或代理每个请求。
- **Console**：基于 React 和 Fastify 的管理控制台，运维人员在这里编写、校验、发布和回滚配置，并确认哪些实例真正运行着这份配置。

配置经由 rnacos（Nacos）从 Console 到达数据面。数据面迁移自 Java 网关 `ploto-unified-access`，并保留了它的配置 wire 格式。

本文介绍产品的功能和运行方式。工作原理见[架构与原理](docs/architecture.md)。

## 项目状态

Access Gateway 正在积极开发中，尚不具备生产使用条件。

- **数据面**：2026-07-31 的有限测试环境快照通过了 352/352 项解码和编译检查，但生产语料差分验证和最终切流门禁尚未完成，[切流门禁](native/access-server/docs/cutover-evidence-gate.md)状态为 `NOT_MET`。
- **Console**：目前唯一可用的身份模式是本地开发管理员，文档加密使用本地密钥。`AUTH_MODE=oidc` 在 provider 适配器实现之前会拒绝启动。

完整清单见[已知限制](#已知限制)。

## 数据面功能

### 监听器与协议

| 监听器      | 默认地址           | 提供                                       |
| ----------- | ------------------ | ------------------------------------------ |
| 网关 TLS    | `0.0.0.0:8443/tcp` | HTTPS，通过 ALPN 协商 HTTP/1.1 和 HTTP/2   |
| 网关 HTTP/3 | `0.0.0.0:8443/udp` | 基于 QUIC 的 HTTP/3，通过 `Alt-Svc` 发布   |
| 明文        | `0.0.0.0:8000/tcp` | HTTP/1.1，可与 TLS 并行，也可作为唯一监听  |
| 状态        | `0.0.0.0:8001`     | Prometheus `/metrics` 和可选的激活证据接口 |

HTTP worker 数量根据进程 CPU affinity 和 cgroup v1/v2 配额自动确定。

### Project 与 Host

一个 Project 拥有一组 Host 模式和一个有序的路由列表。Host 匹配不区分大小写，匹配前去除端口和一个末尾的点，每个模式只能属于一个 Project。

| 模式     | 示例              | 匹配                                                     |
| -------- | ----------------- | -------------------------------------------------------- |
| 精确     | `api.example.com` | 仅该 Host                                                |
| 后缀通配 | `*.example.com`   | `a.example.com` 和 `a.b.example.com`，不含 `example.com` |
| 全局通配 | `*`               | 任意合法 Host                                            |

Project 由 HTTP `Host` 或 `:authority` 选择，与 TLS SNI 无关，所以一张证书可以服务多个 Project，反之亦然。

### 路由匹配

路由先按 Path 结构匹配，再判断可选的 method 和 condition。

| Path 形式  | 示例            | 捕获                        |
| ---------- | --------------- | --------------------------- |
| 静态段     | `/users/me`     | 无                          |
| 参数段     | `/users/:id`    | 一个段，读作 `$path.id`     |
| 尾部通配段 | `/assets/*rest` | 剩余路径，读作 `$path.rest` |

静态段优先于参数段，参数段优先于通配段。同一路径上的候选按配置顺序判断。`condition` 是编译后的表达式，可以读取 `$path`、`$query`、`$header`、`$cookie`、`$context`、`$req` 和 `$conn`：

```yaml
path: /api/orders/:id
method: GET
condition: '$header.x_tenant == "acme"'
type: PROXY
service: orders/stable
```

### 路由类型

**RESPONSE** 由网关直接响应，不访问上游。响应体可以是 `TEXT`、`BASE64`（编译期解码）或 `TEMPLATE`（每个请求求值 `${...}` 表达式）。

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
```

**PROXY** 转发到上游服务，支持路径改写和 header 模板。

```yaml
path: /api/orders/:id
type: PROXY
service: orders/stable
timeout: 30s
rewrite: '/internal/orders/${$path.id}'
proxy_headers:
  X-Tenant: '${$header.x_tenant}'
response_headers:
  Cache-Control: no-store
```

**SCRIPT** 运行类 JavaScript 程序，在配置加载时编译。脚本可以读取请求和 JSON、二进制请求体，设置 header 和 cookie，发送响应，并通过 `http` directive 调用固定的 HTTP 后端。标准库覆盖字符串、数组、二进制、哈希、数学、随机数、JSON 和 URL。它不是 Node.js：没有文件系统、定时器、模块和用户自定义函数。

<!-- prettier-ignore -->
```javascript
return {
    id: $path.id,
    method: $req.method,
    source: $query.source || "unknown"
};
```

### 代理

- **上游**：NamingService 服务发现（`service`，可带 `/cluster` 后缀）或静态 `addresses` 列表。服务发现只使用 enabled、healthy 且权重为正的实例，并优先选择本 zone。
- **负载均衡**：平滑加权轮询，带逐 endpoint 的熔断。
- **重试**：连接失败时重新选择实例，最多尝试 4 次，且仅限请求头尚未开始发送时，所以请求不会被投递两次。
- **连接**：连接池、有界 DNS resolver（没有隐式公共 DNS 回退），以及在单一连接期限内跨解析地址的 Happy Eyeballs。
- **改写**：`rewrite`、`proxy_headers` 和 `response_headers` 模板；不配置 rewrite 时保留原始 URI 和百分号编码。`Location` 和 `Refresh` 自动改写，除非路由显式覆盖。
- **流式传输**：双向流式转发请求体和响应体。`flush: true` 降低 SSE 和慢速上传的延迟。
- **WebSocket**：配置了 `websocket_timeout` 的路由以原始字节隧道转发 HTTP/1.1 升级请求。
- **限制**：路由级 `max_client_body_size`、`max_proxy_body_size` 和 `timeout`，未配置时使用服务默认值（请求 400 MiB，代理响应 600 MiB）。

### 上游 TLS

进程级默认值由 `ACCESS_SERVER_UPSTREAM_TLS_MODE` 决定：`legacy_insecure`（默认，不校验证书）、`system_ca` 或 `custom_ca`。生产环境应使用校验模式。

PROXY 路由可以用不可变的 `upstream_tls` profile 覆盖默认值，设置校验模式、内联 CA、SNI 名称、独立的校验名称和 mTLS 客户端证书。客户端证书通过 Console 证书版本 UUID 引用，私钥不会出现在路由配置中。

### 响应压缩

`gzip: true`（级别 6）或 `gzip: 1`–`9` 可在任意路由类型上启用压缩。静态 `TEXT` 和 `BASE64` 响应体在编译期预压缩，模板、代理和脚本响应以流式压缩。网关按 `Accept-Encoding` 协商，使用固定的 MIME 白名单，合并 `Vary`，在所有表示都被拒绝时返回 406。

### 访问策略

| 策略         | 作用域 | 行为                                                                          |
| ------------ | ------ | ----------------------------------------------------------------------------- |
| HTTPS 重定向 | Host   | 以 301、302、307 或 308 重定向，并发送 `Strict-Transport-Security`            |
| 入口网络     | Host   | 实例入口（`vdi`、`desktop`、`internet`）不被允许时返回 403                    |
| 来源 CIDR    | Route  | `allows` 同时包含 allow 和 `!deny` 项，支持 IPv4 和 IPv6，deny 优先，返回 403 |
| 请求体大小   | Route  | 超限返回 413，声明长度和流式长度都受限                                        |

HTTPS 重定向在明文监听器上生效。在反向代理之后，客户端地址和 scheme 由 `ACCESS_SERVER_CLIENT_METADATA_MODE` 决定：

- `direct`（默认）：使用 socket 对端和监听器真实的 TLS 状态，忽略全部转发头。
- `trusted_proxy`：仅当对端位于 `ACCESS_SERVER_TRUSTED_PROXY_CIDRS` 内时解析 `Forwarded`、`X-Forwarded-For` 和 `X-Real-Ip`。转发链非法时回退到 socket 对端。
- `legacy_headers`：保留 Java 行为，信任这些转发头，且 `X-Real-Ip` 缺失或非法时跳过 CIDR 检查。不要用在非受信客户端可达的监听器上。

### 灰度与 cluster

`ploto.unified-access.gray-match` 中的灰度规则把入口网络映射到比例（万分比）和 CIDR 列表，命中的请求进入 `gray` cluster。cluster 还可以来自路由的 `service/cluster`、`cluster` 字段或 `context` 映射。请求指定的 cluster 没有可用实例时，网关回退到配置的 cluster。

`ACCESS_SERVER_TEST_MODE=true` 时，`api_gray.example.com` 这种 Host 形式按 `api.example.com` 路由并使用 `gray` cluster；Host 未指定 cluster 时由 `HI-TRACE-CLUSTER` 选择。此模式下灰度规则不生效。

### 边缘函数

边缘函数是挂载在一组 Project 上的脚本，可以检查请求，并在它进入路由的常规处理之前直接响应。WAF 基于边缘函数实现，也可以手写函数。

- **阶段**：`request_head` 在路由匹配之前运行，所以未命中路由的路径同样受保护。`request_body` 在匹配之后运行，读取缓冲的 `$req.body`，仅限开启 `edge_function_body: true` 的 PROXY 路由，且限于 JSON、表单、文本和 XML 类型，上限 1 MiB。
- **挂载**：每个挂载包含项目 `include` 和 `exclude` glob、执行 `priority`，以及按客户端 IP 粘性采样的 `ratio`。
- **能力**：读取请求，通过 `resp.*` 提前响应，用 `ctx.report(event, label)` 计数事件，用 `cidr` directive 判断地址。函数不能修改请求，也不能调用上游。
- **失败处理**：运行时错误默认 fail-open。设置 `ACCESS_SERVER_EDGE_FUNCTION_FAIL_CLOSE=true` 后改为返回 503。

```javascript
directive office = cidr "10.0.0.0/8;172.28.1.0/24";

if (!office.contains($conn.remote_addr) && strings.match($req.uri, "(?i)union\\s+select")) {
  ctx.report("waf.block", "sqli-0023");
  resp.send(403, "blocked");
}
```

### 配置热更新

路由、Project 列表、灰度规则、TLS 证书和边缘函数都无需重启即可更新。

- 候选配置完整解析和编译之后，才会替换运行中的快照。
- 非法或超限的候选被拒绝，旧快照继续服务。
- 版本相同的候选被忽略。
- 进行中的请求在开始时的快照上完成。
- 从列表移除 Project 会卸载它，这与更新失败是两回事。

证书在 ClientHello 阶段按 SNI 选择，索引由每张 leaf 证书的 DNS SAN 构建。证书轮换是一次原子的快照切换。

### 可观测性

- **Prometheus**：固定 schema 的指标覆盖请求、延迟、按路由和按上游的流量、代理错误、熔断拒绝、配置结果、TLS 轮换、DNS、服务发现、WebSocket 和边缘函数。请求标签的指标家族有上限，并使用 LRU 逐出。详见[有界指标](native/access-server/docs/bounded-metrics.md)。
- **Grafana**：[`deploy/grafana/`](deploy/grafana/) 提供两个面板：Request Handling 和 Runtime Health。
- **链路追踪**：CAT 请求树、`HI-TRACE-ID` 延续，以及 W3C `traceparent` 和 `tracestate` 传播。
- **访问日志**：与 Java 兼容的制表符分隔格式，每次代理尝试一行，异步写入，100 MB 轮转。日志队列满时丢弃新行，不阻塞请求。
- **边缘函数事件**：每个函数事件写一条独立的结构化日志。

### 升级

`access-server-boss` 托管进程，`access-serverctl` 驱动预热升级。新代在旧代仍在服务时完成配置同步和 worker 初始化，然后接管 TCP 监听。这大幅缩短了服务中断窗口，但不能消除它；两代进程重叠期间 HTTP/3 请求可能失败。详见[升级文档](docs/access-server-prewarm-upgrade-design.md)。

## Console 功能

### Project 与路由

Project 是一个规范化的域名。每条路由有独立的 CodeMirror 编辑器，可以写成 YAML mapping，也可以写成 JavaScript Route（path、method 和 gzip 在源码外设置）。路由支持拖动和键盘排序。保存之前可以预览精确的 native wire 输出，编辑器也会显示 native 资源限额。

### 配置版本

保存会创建新的不可变加密配置版本，由 `If-Match` 乐观锁和幂等键保护。任意版本都可以预览、恢复、作为新编辑的起点，或直接发布。

### Host 与网络策略

- **Host Policy**：固定的主域名，加上共享路由和 Release 的精确 Host 别名，以及 HTTPS 重定向模式。
- **Network Policy**：Project 级的 allow 和 deny CIDR 列表，编译进每条路由。

### 校验

配置分三层检查：字段语法、跨资源关系，以及 Native Validator 的完整编译。Native Validator 与数据面运行同一份 codec 和编译器。创建 Release 要求 validator 可用，所以未经验证的配置无法发布。

### Release 与发布

- Release 从当前版本或任意历史版本创建，在发布之前不可变地落库。回滚就是发布一个旧版本。
- 带租约的 worker 写入每个 rnacos Data ID 并回读，每次尝试单独记录。
- 如果 rnacos 已偏离 Release 创建时的基线，发布以冲突终止，不会覆盖。
- 下线 Project 是单独的 Release 类型，必须记录原因。

### 激活证据

已发布不等于已生效，所以 Console 分开跟踪这两件事。采集器轮询每个实例带鉴权的 `/v1/activation-evidence` 接口，把结果聚合到 Release 页面，并可按需查看逐实例详情。

| 状态       | 含义                                       |
| ---------- | ------------------------------------------ |
| `active`   | 所有必需实例的证据新鲜，且精确匹配 Release |
| `pending`  | 证据新鲜，但目标尚未进入活动快照           |
| `degraded` | 采集失败，或实例拒绝了候选                 |
| `unknown`  | 证据缺失或过期，绝不显示为成功             |

### TLS 证书

- **证书库存**：加密存储的证书，版本不可变，状态为 `valid`、`expiring`（30 天内到期）、`expired` 或 `superseded`。没有私钥下载接口。
- **自动 SNI**：选择器来自证书的 DNS SAN，无需维护单独的绑定。预览功能显示某个 server name 解析到哪张证书。
- **续期**：SAN 不变时直接切换。增减 SAN 会改变 SNI 覆盖范围，需要显式确认。
- **TLS Release**：完整的证书快照像其他 Release 一样发布和跟踪。

### WAF

WAF 规则在 Console 中编写并编译成边缘函数，数据面没有任何 WAF 专用逻辑。

- **规则组**：每个组有模式（`off`、`log` 或 `block`）、优先级，以及生效和豁免的 Project。豁免必须填写原因。每个生效的组发布为一个名为 `waf-<组>` 的边缘函数。
- **规则**：匹配表达式，加上严重级别、动作，以及可选的 method、path 和来源 CIDR 范围。
- **Project 覆盖**：按 Project 设置模式，覆盖它所属的各组。
- **起手规则包**：六个组共九条规则（SQL 注入、RCE、XSS、路径穿越、探测、畸形请求）。导入时各组都是 `off`，启用之前不改变任何行为。
- **拦截**：被拦截的请求收到 403 JSON 响应，包含代码 `WAF_BLOCKED` 和规则 ID。
- **发布**：编译预览检查生成的脚本，发布预览把它们与 rnacos 上的线上内容逐函数比较。回滚从历史内容创建新的 Release。
- **命中统计**：按规则统计 1 小时、24 小时或 7 天内的 `log` 和 `block` 次数，数据来自 Prometheus。此页面需要配置 `PROMETHEUS_URL`。

推荐的上线顺序是：先在单个 Project 上用 `log` 模式，查看命中统计，再切换到 `block`。

### 手写边缘函数

专门的页面用于编辑函数脚本、阶段和挂载，并提供逐函数的 native 编译预览。手写函数和 WAF 生成的函数一起发布。`waf` 和 `waf-*` 是生成器的保留名，一份快照最多 64 个函数。

### 权限与审计

- **角色**：`admin` 和 `maintainer` 编辑配置；`admin` 和 `publisher` 创建和发布 Release；`auditor` 只读。
- **审计**：配置、证书和发布变更都写入脱敏的审计事件。
- **加密**：配置文档和私钥以 AES-256-GCM 信封加密存储。

### 内置文档

路由、脚本和脚本 API 指南在 Console 的 `/docs` 下提供，有简体中文和英文版本。

## 已知限制

**Console**

- OIDC 尚未实现，因此无法进行生产登录。本地文档密钥仅用于开发；生产需要外部密钥提供方，目前也不存在。
- 灰度规则和 Host 入口网络限制不能在 Console 中编辑，需要直接在 rnacos 中编写。
- 失败或部分成功的发布是终态，没有自动的逐资源重试。恢复方式是创建新的 Release。
- TLS Release 没有 WAF Release 那样的一键回滚。
- 审计事件写入了数据库，但还没有查看页面或 API。
- 角色成员关系没有管理页面或 API。
- 已删除边缘函数的脚本 Data ID 会作为无害的孤儿留在 rnacos 中。

**数据面**

- 上游连接只支持 HTTP/1.1，WebSocket 要求客户端使用 HTTP/1.1 升级。
- JavaScript Route 以 413 拒绝长度未知的请求体，其 `http` directive 不解析 NamingService 上游。
- 边缘函数不能改写请求，脚本 VM 没有指令预算，目前依靠语言的静态限制和输入限额保护。
- `/metrics` 没有鉴权，状态监听器应放在私有网络上。
- 修改 DNS resolver 文件、进程 CA bundle 或激活 token 需要滚动重启。

## 快速开始：Docker 演示

演示环境运行完整的技术栈：Console 和 API、发布 Worker、激活证据采集器、MySQL 8.4、R-Nacos 和 `access-server`。它需要 Docker Compose、OpenSSL 和[开发环境要求](#环境要求)中的工具，因为二进制文件先在本地编译。

```bash
git clone --recurse-submodules https://github.com/fiber-net-gateway/access-gateway.git
cd access-gateway
npm install
npm run demo:init   # 生成本地密钥和自签名证书
npm run demo:up     # 本地构建，然后启动整个环境
```

首次构建会下载 native 依赖，可能需要几分钟；`NATIVE_BUILD_JOBS` 设置并行度（默认 2）。Compose 会执行 migration，上传演示证书，通过 Console 发布 TLS 快照和演示 Project，两者都进入 R-Nacos 后启动 `access-server`。

| 服务             | 地址                                                            |
| ---------------- | --------------------------------------------------------------- |
| Console          | `http://localhost:8088`                                         |
| 网关 HTTPS/HTTP2 | `curl -k --http2 -H 'Host: demo.local' https://localhost:8443/` |
| 网关明文         | `curl -H 'Host: demo.local' http://localhost:8000/`             |
| 指标             | `http://localhost:8001/metrics`                                 |
| R-Nacos 控制台   | `http://localhost:10848/rnacos/`                                |
| MySQL            | `127.0.0.1:3307`                                                |

凭据保存在被忽略的本地 `.env` 文件中。用 `npm run demo:ps`、`npm run demo:logs` 和 `npm run demo:down` 操作环境。只有想删除全部演示数据时，才给 `docker compose down` 加 `--volumes`。

在 WSL2 下从 Windows 浏览器测试 HTTP/3 时，要使用 `hostname -I` 得到的 WSL 地址，因为 WSL2 通常只转发 Windows `localhost` 的 TCP，不转发 UDP。把这个地址映射到 Windows hosts 文件中的 `demo.local`，并信任演示证书。不要在本地测试之外复用这张证书。在 `.env` 中设置 `ACCESS_SERVER_PUBLISHED_HOST=127.0.0.1` 可以让网关只在 WSL 内部可达。

## 开发

### 环境要求

- **Console**：Node.js 20.19 或更高版本，npm 支持 workspaces。
- **Native（Linux）**：CMake 4.1 或更高版本、Ninja，以及支持 C++23 的 Clang 17+ 或 GCC 13+。

克隆时使用 `--recurse-submodules`，已有 checkout 执行 `git submodule update --init --recursive`。不要使用 `git submodule update --remote`：Fiber 是固定版本，更新要通过评审过的 gitlink 变更完成。

### Console

```bash
npm install
cp server/.env.example server/.env
npm run dev
```

这会启动 Console（`http://localhost:5173`）和 API（`http://127.0.0.1:3000`），Vite 代理 `/api`。用 `npm run dev:web` 或 `npm run dev:server` 可以只运行一侧。

没有 MySQL 时，持久化接口返回 `503`，`/api/health/ready` 报告 degraded。要启用持久化，先创建 MySQL 8.4 数据库，然后在 `server/.env` 中设置 `MYSQL_ENABLED=true`、`MYSQL_PASSWORD` 和 `DOCUMENT_ENCRYPTION_KEY_BASE64`（由 `openssl rand -base64 32` 生成）：

```bash
npm run db:migrate
npm run dev
```

API 不会自动执行 migration。新数据库没有工作区，需要发一次 `POST /api/environments` 创建；请求示例见 [`deploy/demo/bootstrap-console.mjs`](deploy/demo/bootstrap-console.mjs)。

| 能力         | 配置                                                                                                                      |
| ------------ | ------------------------------------------------------------------------------------------------------------------------- |
| Native 校验  | `NATIVE_VALIDATOR_PATH`，指向 `access-gateway-validator` 的绝对路径                                                       |
| 发布 Worker  | `PUBLICATION_WORKER_ENABLED=true`，用 `npm run publication-worker --workspace server` 运行                                |
| 激活证据     | `ACTIVATION_COLLECTOR_ENABLED=true`、`ACTIVATION_TARGETS_JSON`，用 `npm run activation-collector --workspace server` 运行 |
| WAF 命中统计 | `PROMETHEUS_URL`                                                                                                          |

Worker 和采集器从构建产物运行，所以要先执行 `npm run build`。全部配置项见 [`server/.env.example`](server/.env.example)。

### Native

```bash
npm run configure:native
npm run build:native             # native/build/apps/access-server
npm run build:native-validator   # native/build/apps/access-gateway-validator
npm run test:native
```

其他命令：`npm run test:native:sanitizers`、`npm run test:native:interop`、`npm run benchmark:native` 和 `npm run verify:native:cutover`。

### 验证

| 变更范围                | 运行                                                                     |
| ----------------------- | ------------------------------------------------------------------------ |
| `web/`、`server/`       | `npm run typecheck`、`npm test`、`npm run format:check`、`npm run build` |
| `native/access-server/` | `npm run test:native`、`npm run format:native`，并检查 diff              |

完整矩阵和全部编码规范见 [AGENTS.md](AGENTS.md)。

## 运行 access-server

```bash
cp native/access-server/access-server.env.example access-server.env
# 设置 NACOS_SERVER_ADDRESSES 和 ACCESS_SERVER_NETWORK_ENTRY（见下文）。
./native/build/apps/access-server access-server.env
```

不带参数时，进程读取当前目录的 `access-server.env`。格式是严格的 `KEY=VALUE`，未知或重复的键会使启动失败。不要提交这个文件。

开始服务之前，进程最多等待 `ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS`，直到拿到有效的 Project 列表和它引用的全部路由；启用 TLS 时还需要有效的证书快照。之后绑定监听器、注册到 Nacos，注册成功后开始接收流量。配置缺失或非法、证书过期或绑定失败都会终止启动。

| 领域       | 配置                                                                                                                   |
| ---------- | ---------------------------------------------------------------------------------------------------------------------- |
| 必填       | `NACOS_SERVER_ADDRESSES`（IP 字面量）、`ACCESS_SERVER_NETWORK_ENTRY`                                                   |
| 监听器     | `ACCESS_SERVER_TLS_*`、`ACCESS_SERVER_HTTP3_ENABLED`、`ACCESS_SERVER_PLAIN_LISTEN_*`、`ACCESS_SERVER_METRICS_LISTEN_*` |
| 客户端信任 | `ACCESS_SERVER_CLIENT_METADATA_MODE`、`ACCESS_SERVER_TRUSTED_PROXY_CIDRS`                                              |
| 上游       | `ACCESS_SERVER_UPSTREAM_*`、`ACCESS_SERVER_HAPPY_EYEBALLS_*`、`ACCESS_SERVER_DNS_*`                                    |
| 限制       | `ACCESS_SERVER_MAX_REQUEST_BODY_SIZE`、`ACCESS_SERVER_MAX_RESPONSE_BODY_SIZE`                                          |
| 边缘函数   | `ACCESS_SERVER_EDGE_FUNCTION_FAIL_CLOSE`（默认 `false`）                                                               |
| 激活证据   | `ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED`、`ACCESS_SERVER_INSTANCE_ID`、`ACCESS_SERVER_ACTIVATION_TOKEN`             |
| 注册       | `ACCESS_SERVER_SERVICE_NAME`、`ACCESS_SERVER_CLUSTER`、`ACCESS_SERVER_ADVERTISE_ADDRESS`、`ACCESS_SERVER_ZONE`         |
| 日志与 CAT | `ACCESS_SERVER_ACCESS_LOG_DIR`、`CAT_*`                                                                                |

`ACCESS_SERVER_NETWORK_ENTRY` 声明该实例服务的入口网络（例如 `internet`、`vdi` 或 `desktop`），缺失时进程拒绝启动。实例根据这个值和连接信息设置 `X-Entry`、`X-Real-Ip`、`X-Forwarded-Proto` 和 `X-Forwarded-For`，并覆盖客户端提交的 `X-Entry`。`internet` 入口还会信任 CDN 的 `EO-Connecting-IP` 作为 `X-Real-Ip`。

只运行明文时，设置 `ACCESS_SERVER_TLS_ENABLED=false` 和 `ACCESS_SERVER_HTTP3_ENABLED=false`。

默认值和各配置项说明见 [`access-server.env.example`](native/access-server/access-server.env.example)。

## rnacos 契约

| 用途                    | Data ID                                     | Group           |
| ----------------------- | ------------------------------------------- | --------------- |
| Project 列表            | `ploto.unified-access.projects`             | `ACCESS-SERVER` |
| Project 路由            | `ploto.unified-access.route.<project>`      | `ACCESS-SERVER` |
| TLS 证书                | `ploto.unified-access.tls-certificates`     | `ACCESS-SERVER` |
| 边缘函数索引            | `ploto.unified-access.edge-functions`       | `ACCESS-SERVER` |
| 边缘函数脚本            | `ploto.unified-access.edge-function.<name>` | `ACCESS-SERVER` |
| 灰度规则                | `ploto.unified-access.gray-match`           | `DEFAULT_GROUP` |
| 测试环境 cluster 白名单 | `ploto.test.temp-cluster-apps`              | `DEFAULT_GROUP` |

Project 列表是分号分隔的字符串，不是 JSON。可接受的值和默认值以 native codec 为准。`ploto.test.temp-cluster-apps` 只在 `ACCESS_SERVER_TEST_MODE=true` 时读取。

## 仓库结构

```text
access-gateway/
├── native/access-server/        # C++23 数据面、测试和 native 文档
├── server/                      # Fastify 控制面 API、Worker 和采集器
├── web/                         # React 和 Vite 控制台
├── deploy/                      # Dockerfile、演示 bootstrap、Grafana 面板
├── docs/                        # 架构、设计文档和用户指南
├── third_party/fiber-gateway-cpp/  # 固定版本的只读 Fiber 子模块
└── AGENTS.md                    # 开发规范
```

## 文档

**从这里开始**

- [架构与原理](docs/architecture.md)
- [用户指南索引](docs/user-guide/README.md)，提供简体中文和英文版本：[路由](docs/user-guide/zh-CN/routing.md)、[脚本路由](docs/user-guide/zh-CN/script-routing.md)和[脚本参考](docs/user-guide/zh-CN/script-reference.md)

**设计**

- [边缘函数与 WAF](docs/edge-functions-and-waf-design.md)
- [实例激活证据](docs/activation-evidence.md)
- [网络策略与证书](docs/network-policy-and-certificate-design.md)
- [预热升级](docs/access-server-prewarm-upgrade-design.md)
- [Console 需求](docs/console-requirements.md)和[详细设计](docs/console-detailed-design.md)

**数据面**

- [Access Server 指南](native/access-server/README.md)
- [兼容契约](native/access-server/docs/compatibility-contract.md)
- [配置编译](native/access-server/docs/config-compilation.md)和[资源上限](native/access-server/docs/config-resource-limits.md)
- [有界指标](native/access-server/docs/bounded-metrics.md)
- [性能基准](native/access-server/docs/benchmark-baselines.md)
- [迁移计划](native/access-server/docs/migration-plan.md)和[切流证据门禁](native/access-server/docs/cutover-evidence-gate.md)
- [上游来源](native/access-server/UPSTREAM.md)

## 许可证

本项目采用 [Apache License 2.0](LICENSE) 许可。迁移的应用保留其[上游许可声明](native/access-server/LICENSE.upstream)。
