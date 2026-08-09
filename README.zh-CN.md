# Access Gateway

[English](README.md) | 简体中文

Access Gateway 是由 C++23 数据面与 Web 管理控制台组成的高性能网关产品。数据面迁移自
[`fiber-gateway-cpp/apps/access-server`](https://github.com/fiber-net-gateway/fiber-gateway-cpp/tree/master/apps/access-server)，
并继续使用固定版本的 Fiber 框架作为运行时基础。控制面前后端均采用 TypeScript，前端
使用 React，后端使用 Node.js。

## 项目状态

本仓库同时持续开发两个一等组件：

- **Access Server**：原生运行时已支持 Nacos 驱动的项目与路由配置、Host/Path/条件
  匹配、RESPONSE 与 PROXY 执行、WebSocket 隧道、服务发现、灰度路由、CAT 链路追踪、
  Prometheus 指标和结构化访问日志。生产脚本语料差分验证与最终切流门槛仍在推进中。
- **Console**：首个基于 MySQL 的纵向链路已经可用，包括确定性 migration、开发身份与环境
  RBAC、环境/项目 API、加密的不可变草稿 revision、乐观锁、审计事件，以及 React 环境/项目
  工作台。Release/发布表、Release 状态机、发布冲突判定和 fail-closed Native Validator
  子进程适配器已经落地。OIDC、Nacos 发布 Worker、完整路由编辑器和实例级生效采集仍待实现。

因此，对于尚无配套工作流的状态，控制台会明确显示“不可用”或“未知”，不会伪造发布或
生效成功。

## 架构

```mermaid
flowchart LR
    User[运维人员] --> Web[React Console]
    Web --> API[Node.js / Fastify API]
    API --> DB[(控制面数据库)]
    API --> Nacos[(Nacos / rnacos)]
    Nacos --> Access[原生 Access Server]
    Discovery[Nacos NamingService] --> Access
    Client[网关客户端] --> Access
    Access --> Upstream[上游服务]
    Access --> Obs[CAT / Prometheus / 日志]
```

Console 负责配置编写和发布流程，只有 `access-server` 位于网关流量链路中。草稿、已发布
和已生效是相互独立的状态：Nacos 写入成功不代表某个服务实例已经启用新快照。

## 仓库结构

```text
access-gateway/
├── native/
│   ├── CMakeLists.txt             # 原生构建与固定版本 Fiber 集成
│   └── access-server/             # 本仓库维护的 C++23 数据面
├── server/                        # TypeScript、Node.js、Fastify Console API
├── web/                           # TypeScript、React、Vite Console 前端
├── third_party/
│   └── fiber-gateway-cpp/         # 固定版本、只读的 Git 子模块
├── AGENTS.md                      # 仓库开发规范
└── package.json                   # 根 npm 工作区与统一命令
```

应用特有的数据面行为放在 `native/access-server`。可复用的 Fiber 运行时、HTTP、Nacos、
CAT、Prometheus、JSON 和脚本组件来自固定版本的子模块。

## 环境要求

Console 开发需要：

- Node.js 20.19 或更高版本；
- 支持 workspace 的 npm。

原生代码目前面向 Linux，需要：

- CMake 4.1 或更高版本；
- Ninja；
- Clang 17 或更高版本，或者 GCC 13 或更高版本；
- 支持 C++23 的工具链。

首次配置原生构建时可能下载第三方构建依赖。运行 CMake 前必须先初始化固定版本的子模块。

## 克隆仓库

同时克隆仓库和 Fiber 依赖：

```bash
git clone --recurse-submodules https://github.com/fiber-net-gateway/access-gateway.git
cd access-gateway
```

对于已有工作区：

```bash
git submodule update --init --recursive
```

日常构建不要使用 `git submodule update --remote`；依赖升级应作为明确的 gitlink 变更进行
审查和提交。

## Console 开发

安装根工作区依赖，创建后端本地配置，然后同时启动前后端开发服务：

```bash
npm install
cp server/.env.example server/.env
npm run dev
```

默认开发地址：

- Console：`http://localhost:5173`
- Console API：`http://127.0.0.1:3000`
- 健康检查：`http://127.0.0.1:3000/api/health`

Vite 会把浏览器的 `/api` 请求代理到 Fastify。需要单独运行一端时，使用
`npm run dev:web` 或 `npm run dev:server`。

API 可以在未配置 MySQL 时启动，便于开发前后端联通；此时持久化接口返回 `503`，
`/api/health/ready` 会报告降级。要启用持久化，请创建 MySQL 8.4 数据库，在
`server/.env` 中设置 `MYSQL_ENABLED=true`、`MYSQL_PASSWORD` 和本地 32 字节文档密钥，
然后先执行 migration：

```bash
openssl rand -base64 32
# 把结果写入 server/.env 的 DOCUMENT_ENCRYPTION_KEY_BASE64。
npm run db:migrate
npm run dev
```

Migration 使用 checksum 防篡改，并通过 MySQL advisory lock 串行执行；API 不会自动迁移。
`AUTH_MODE=development` 会建立本地平台管理员，且在 `NODE_ENV=production` 下被拒绝。
生产 OIDC provider adapter 完成前，OIDC 模式会拒绝启动。

目前实现的控制面接口包括：

- `/api/health`、`/api/health/live`、`/api/health/ready` 和 `/api/system/status`；
- 环境的列表/创建/详情，以及项目的列表/创建/详情；
- 项目活动草稿的获取/创建、带 `If-Match` 的不可变 revision 保存与 revision 获取。

草稿模型在写入 MySQL 前使用 AES-256-GCM 信封加密。本地密钥配置只用于开发环境；生产
环境应接入外部密钥服务。

使用以下命令验证全部 Console 工作区：

```bash
npm run typecheck
npm test
npm run format:check
npm run build
```

## 本机 Docker 演示

仓库提供了完整的 Docker Compose 演示环境，包含合并部署的 React Console 与 Fastify API、
MySQL 8.4、R-Nacos 和原生 Access Server。先生成仅保存在本机的随机 Secret，再启动全部服务：

```bash
npm run demo:init
npm run demo:up
```

首次构建原生镜像会下载固定版本的 C++ 构建依赖，可能需要几分钟。Compose 会自动执行
MySQL migration，通过 Console API 幂等创建演示环境和项目，将兼容路由直接写入 R-Nacos，
并在 bootstrap 成功后启动 Access Server。

Fastify 在同一个 Console 容器中同时提供 `/api/*` 接口和 React 单页应用。完整环境因此包含
4 个长期运行容器和 3 个 migration/bootstrap 一次性容器。

全部服务就绪后可访问：

- Console：`http://localhost:8088`
- Access Server 演示：`curl -H 'Host: demo.local' http://localhost:16688/`
- Access Server 指标：`http://localhost:16689/metrics`
- R-Nacos Console：`http://localhost:10848/rnacos/`
- MySQL：`127.0.0.1:3307`

R-Nacos Console 凭据和数据库 Secret 只保存在 Git 已忽略的本机 `.env` 文件中。由于 Console
发布 Worker 尚未实现，bootstrap 会直接写入 R-Nacos；UI 仍会如实把发布与实例生效状态显示为
“不可用”或“未知”。可使用 `npm run demo:ps`、`npm run demo:logs` 和 `npm run demo:down`
管理演示环境。只有在确定要删除全部演示数据时，才为 `docker compose down` 添加 `--volumes`。

## 构建和测试 Access Server

根工作区提供了常用的原生开发流程：

```bash
npm run configure:native
npm run build:native
npm run build:native-validator
npm run test:native
```

等价的 CMake 命令是：

```bash
cmake -S native -B native/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_BUILD_TESTS=ON
cmake --build native/build --target fiber_app_access_server --parallel
cmake --build native/build --target fiber_app_access_gateway_validator --parallel
cmake --build native/build --target fiber_access_server_tests --parallel
ctest --test-dir native/build --output-on-failure -L access-server
```

可执行文件输出到 `native/build/apps/access-server`。
离线控制面校验器输出到 `native/build/apps/access-gateway-validator`，通过绝对路径
`NATIVE_VALIDATOR_PATH` 配置。它从 stdin 读取一条有大小上限、带版本的 JSON 请求，并在
stdout 返回一条 JSON 结果；校验直接复用原生 codec、脚本编译器和 compiled route model，
不会连接 Nacos 或外部服务。

## 运行 Access Server

复制原生示例配置，并至少按环境修改 Nacos 地址：

```bash
cp native/access-server/access-server.env.example access-server.env
./native/build/apps/access-server access-server.env
```

未传参数时，进程读取当前目录下的 `access-server.env`。默认监听地址为：

- 网关 HTTP：`0.0.0.0:16688`；
- Prometheus 指标：`0.0.0.0:16689`。

配置文件采用严格的 `KEY=VALUE` 格式，未知键和重复键都会导致启动失败。不要提交本地环境
文件或凭据。全部配置项参见
[`native/access-server/access-server.env.example`](native/access-server/access-server.env.example)。

## Nacos 配置契约

原生 codec 是输入值、默认值和更新行为的事实来源。默认兼容契约如下：

| 用途         | Data ID                                | Group           |
| ------------ | -------------------------------------- | --------------- |
| 项目列表     | `ploto.unified-access.projects`        | `ACCESS-SERVER` |
| 项目路由     | `ploto.unified-access.route.<project>` | `ACCESS-SERVER` |
| 生产灰度规则 | `ploto.unified-access.gray-match`      | `DEFAULT_GROUP` |

项目列表是分号分隔的字符串，而不是 JSON。路由和灰度规则载荷保持 Java 兼容的 wire 行为。
候选配置必须先完成全部解析和编译，再以不可变快照发布；候选无效时继续保留上一份生效快照。

## 文档

- [Console 产品需求](docs/console-requirements.md)
- [Console 详细设计](docs/console-detailed-design.md)
- [Access Server 指南](native/access-server/README.md)
- [兼容性契约](native/access-server/docs/compatibility-contract.md)
- [迁移计划](native/access-server/docs/migration-plan.md)
- [脚本语料差分状态](native/access-server/docs/script-corpus-differential.md)
- [上游来源记录](native/access-server/UPSTREAM.md)
- [开发规范](AGENTS.md)

## 许可证

本项目使用 [Apache License 2.0](LICENSE)。迁移的应用代码保留了对应的
[上游许可证声明](native/access-server/LICENSE.upstream)。
