# 固定工作区、域名项目与安全监听器设计

> 本文记录变更前的固定工作区、整份 JSON 草稿编辑器和仍在使用的启动证书实现。Console 产品交互已由
> [Access Gateway Console 产品需求文档](console-requirements.md) 取代：不再展示工作区/环境，
> 路由改为逐条 YAML 编辑器，证书成为一等产品对象。

## 1. 目标

本次变更把 Console 的用户模型从“创建环境，再在环境中创建项目”收敛为：

1. 一个部署只有一个固定工作区；
2. 用户创建的项目就是一个规范化域名；
3. 每个域名项目创建时自动建立路由草稿，用户可继续配置完整 Host/Route 模型；
4. Access Server 从 PEM 证书和私钥启动安全监听器，默认启用 HTTPS、HTTP/2 和 HTTP/3；
5. Docker Compose 演示自动生成仅用于本机的自签名证书，并同时发布 TCP/UDP 端口。

发布 Worker 和激活回执不在本次范围内。Console 保存的是 MySQL 中的草稿修订，不把“保存”伪装成
“已发布”；演示环境仍由明确的 R-Nacos bootstrap 提供初始在线配置。

## 2. 固定工作区

### 2.1 数据模型

保留 `environments`、`environment_memberships` 及其外键，不做破坏性迁移。环境记录继续承担：

- Nacos endpoint、namespace、tenant 与 Data ID 契约；
- 权限、审计、加密文档和发布序列的隔离根；
- 未来发布 Worker 的资源作用域。

产品层只暴露一个固定工作区：

- `GET /api/workspace` 返回当前用户可访问、最早创建的环境；
- 没有工作区时返回 404；历史版本遗留的其他环境不再出现在 Web 的选择流程中；
- `POST /api/environments` 仅保留为部署 bootstrap 接口，并拒绝创建第二个环境；
- Web 不提供环境创建、列表选择或切换入口。

这样可以简化日常操作，同时避免丢失现有数据库约束和未来发布能力。

## 3. 域名项目与路由草稿

### 3.1 域名规范

API 使用 `domain` 字段，数据库继续复用 `projects.name` 作为 wire project name。服务端执行：

- 去除首尾空白和一个末尾根域点；
- 使用 WHATWG/UTS #46 `domainToASCII` 转成 ASCII；
- 转小写；
- 总长不超过 253，每个 label 为 1..63 字符；
- label 只允许字母、数字和中划线，且不能以中划线开头或结尾；
- 不接受通配符、端口、URL、路径或 IP 字面量。

环境内域名使用现有唯一索引去重。响应把该值作为 `domain` 返回，不再向用户展示抽象项目名。

### 3.2 创建与编辑

创建域名项目在同一个 MySQL 事务中写入：

- `projects`；
- `project_version_counters`；
- 一个 `editing` 状态、revision 为 0 的 `project_route` 草稿；
- project/draft 两条审计事件。

初始编辑模型为：

```json
{
  "schemaVersion": 1,
  "kind": "project_route",
  "hosts": [{ "pattern": "api.example.com" }],
  "routes": []
}
```

Web 提供项目选择和完整 JSON 模型编辑器。JSON 方式不会裁剪 RESPONSE、PROXY、condition、
rewrite、headers、CIDR、body limit 或 WebSocket 等高级字段；前端进行 JSON/外层 schema 检查，后端仍
执行同样的 schema 和乐观锁校验。后续可在不改变 API 模型的情况下逐步增加结构化表单。

新增 `GET /api/drafts/:draftId/current-revision`：revision 为 0 时返回 404；否则返回当前修订和
模型。保存继续使用 `If-Match`，成功后 ETag 增加。

## 4. Access Server TLS、HTTP/2 与 HTTP/3

### 4.1 配置契约

新增严格配置项：

| 配置                                     | 默认值                                  | 说明                          |
| ---------------------------------------- | --------------------------------------- | ----------------------------- |
| `ACCESS_SERVER_TLS_ENABLED`              | `true`                                  | 安全监听器开关                |
| `ACCESS_SERVER_TLS_CERTIFICATES_DATA_ID` | `ploto.unified-access.tls-certificates` | 完整 TLS 快照 Data ID         |
| `ACCESS_SERVER_TLS_CERTIFICATES_GROUP`   | `ACCESS-SERVER`                         | 完整 TLS 快照 Group           |
| `ACCESS_SERVER_HTTP3_ENABLED`            | `true`                                  | 在相同地址和端口绑定 QUIC UDP |

HTTP/2 不设置独立开关：Fiber `HttpServer` 在 TLS 模式固定通过 ALPN 提供 `h2` 和
`http/1.1`，避免出现“配置显示开启但协议未通”的状态。HTTP/3 开启但 TLS 关闭属于非法配置。

TLS 默认开启，因此旧的纯 HTTP 部署必须显式设置 `ACCESS_SERVER_TLS_ENABLED=false` 和
`ACCESS_SERVER_HTTP3_ENABLED=false`。TLS 模式会先订阅并完整校验 Nacos 证书快照；首值缺失、
证书/私钥不匹配、证书不在有效期、TCP 或 UDP 任一绑定失败时启动失败，不静默降级到 HTTP。
原文件证书配置项已经移除，避免形成两套证书来源。

### 4.2 监听与发现

`ACCESS_SERVER_LISTEN_PORT` 同时表示：

- HTTPS over TCP：HTTP/1.1 与 HTTP/2；
- HTTP/3 over UDP。

Access Server 使用 Fiber 的 `HttpServer` 替换仅支持明文的 `Http1Server`。HTTP/3 开启时，所有
响应携带 `Alt-Svc: h3=\":port\"; ma=86400`，供浏览器从 HTTP/1.1/2 发现 QUIC 端点。Metrics
继续使用独立的明文 HTTP/1.1 监听器，避免监控系统被业务证书耦合。

直连 TLS 请求以 `exchange.scheme() == "https"` 判断 HTTPS，同时保留受信任反向代理传入的
`X-Forwarded-Proto: https` 兼容路径，防止安全监听器上的 HTTPS redirect 循环。

### 4.3 证书安全

- Console 将证书链、私钥和 Release payload 分别 envelope-encrypted 保存，API、日志和审计均不回显；
- TLS Release 的完整 JSON payload 通过 Nacos 交付；必须对 Nacos 使用受控网络、鉴权和加密传输；
- Access Server 在更新线程内解析 PEM，运行快照只保留编译后的 TLS context 和 SAN 索引；
- 启动桥接使用只读 sealed memfd，监听器初始化后立即关闭，不把私钥写入磁盘或镜像层；
- `deploy/demo/init-env.sh` 使用 `umask 077` 生成自签名 ECDSA P-256 证书，SAN 覆盖
  `demo.local`、`localhost`、`127.0.0.1` 和 `::1`；
- `.env` 和 `deploy/demo/certs/` 均被忽略。自签名证书仅用于演示，生产环境应挂载由受信 CA
  签发的证书并独立管理私钥权限。

## 5. Docker Compose 演示

- 对外仍使用一个 Access Server 端口，Compose 同时发布同端口的 TCP 和 UDP；
- 16688 默认绑定全部 WSL 接口，使 Windows 可通过 WSL IP 直连 UDP；需要限制为 WSL 内部访问时，
  可将 `ACCESS_SERVER_PUBLISHED_HOST` 设置为 `127.0.0.1`；
- 健康检查改为忽略本机自签名证书的 HTTPS 请求；
- R-Nacos、MySQL、Console 和一次性 bootstrap 容器数量不因此增加；
- 验收至少覆盖：Console 工作区 API、创建域名及草稿、HTTPS HTTP/1.1、ALPN `h2`、HTTP/3
  原生客户端测试、UDP 端口发布和 Metrics。

## 6. 兼容性与失败策略

- 数据库 schema 不删除环境表，也不需要搬迁现有项目；已有项目若不是合法域名，仍可读取，但新建
  项目必须满足域名规范；
- 若数据库已有多个历史环境，固定工作区 API 稳定选择最早创建且当前用户可访问的记录；
  不自动删除历史数据，后续数据合并由独立迁移处理；
- 草稿保存不等于发布，UI 始终显示真实状态；
- HTTP/3 依赖 UDP 可达性。TCP HTTPS 正常而 UDP 被防火墙拦截时，客户端会回退到 HTTP/2，服务端
  不把该网络状态误报为协议未启用。
