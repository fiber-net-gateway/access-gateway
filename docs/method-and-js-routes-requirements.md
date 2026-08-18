# Method 与 JavaScript Route 需求

## 1. 背景与目标

当前 Project Route 由 Console 中按顺序保存的 YAML 条目编译为 rnacos 的
`ploto.unified-access.route.<project>` JSON，并由 access-server 构建不可变路由快照。
本次变更在保留 Host、Path、condition、版本、发布和失败保留旧快照语义的前提下，增加：

1. Route 可按一个 HTTP method 精确匹配；
2. Route 条目除 YAML 外还可以使用请求期 JavaScript；
3. 一个 Project 的有序 Route 列表可混合 YAML 与 JavaScript 条目；
4. RESPONSE、PROXY 和 JavaScript Route 可选择固定 MIME 白名单上的 gzip 响应。

## 2. 范围

### 2.1 本次包含

- YAML Route 增加可选 `method` 字段；
- JavaScript Route 使用外置 `path`、可选 `method` 和必填脚本正文；
- YAML/JavaScript Route 可选 `gzip: true|false|1..9`；动态响应由请求级共享 writer 流式压缩；
- Console 的保存模型、编辑界面、本地校验、服务端编译器和 Native Validator 支持混合条目；
- access-server wire codec、快照编译、请求匹配和 JS 执行支持新类型；
- 旧 schema、旧 wire payload 和不含 `method` 的 Route 行为不变。

### 2.2 本次不包含

- 多 method 数组、method 通配表达式或大小写归一化；
- 在 JavaScript 正文内声明或修改 `path`、`method`；
- JavaScript Route 的 YAML `condition`、Route 级 CIDR、body limit 或静态 PROXY/RESPONSE 字段；
- JavaScript 的动态上游 HTTP directive。脚本本期只开放标准库、`req.*`、`resp.*` 和
  `$req`/`$conn`/`$path`/`$query`/`$header`/`$cookie`/`$context` 常量；
- 把 Console 变为代理或在 Console/Node 进程中执行用户脚本；
- 以 rnacos 写入/readback 推断实例已激活。

## 3. 术语与模型

- **YAML Route**：正文是一条 YAML mapping。`path`、可选 `method` 和现有执行字段均在正文中。
- **JavaScript Route**：正文只包含请求处理脚本；`path`、可选 `method` 和可选 `gzip` 是条目外置元数据。
- **全部 method**：Route 未配置 method；任何请求 method 都可进入该候选。
- **混合列表**：同一 Project 的 Route 列表中 YAML 与 JavaScript 条目共同按列表顺序参与匹配。

## 4. 功能需求

### 4.1 Method 匹配

| 编号             | 优先级 | 需求                                                                                            |
| ---------------- | ------ | ----------------------------------------------------------------------------------------------- |
| ROUTE-METHOD-001 | P0     | YAML Route 可声明一个可选字符串字段 `method`                                                    |
| ROUTE-METHOD-002 | P0     | JavaScript Route 可在正文外声明一个可选 `method`                                                |
| ROUTE-METHOD-003 | P0     | 未声明或 wire 值为 `null` 时匹配全部 method                                                     |
| ROUTE-METHOD-004 | P0     | 声明后按 HTTP method 原始字节精确匹配，大小写敏感，不自动转大写                                 |
| ROUTE-METHOD-005 | P0     | method 必须是非空 RFC HTTP token，Console 和 native 都必须拒绝非法值                            |
| ROUTE-METHOD-006 | P0     | 同一 path 可按不同 method 安装多条 Route；method 不匹配时继续尝试同 path 的后续候选             |
| ROUTE-METHOD-007 | P0     | method 与现有 condition 同时存在时执行 AND 语义：先比 method，再执行 condition                  |
| ROUTE-METHOD-008 | P0     | 一个更早的无 method、无 condition Route 仍会使同 path 后续 Route 成为 dead route 并拒绝候选快照 |
| ROUTE-METHOD-009 | P0     | 返回 404 路由未匹配，不新增隐式 405 或 `Allow` 响应                                             |

推荐使用大写标准 method（如 `GET`、`POST`），但为兼容扩展 method 不维护固定枚举。

### 4.2 JavaScript Route

| 编号         | 优先级 | 需求                                                                                                |
| ------------ | ------ | --------------------------------------------------------------------------------------------------- |
| ROUTE-JS-001 | P0     | JavaScript Route 必须同时具有非空脚本正文和非空外置 `path`                                          |
| ROUTE-JS-002 | P0     | JavaScript Route 的外置 `method` 可省略；省略即匹配全部 method                                      |
| ROUTE-JS-003 | P0     | JavaScript 正文不能充当 `path`/`method` 配置来源                                                    |
| ROUTE-JS-004 | P0     | 脚本必须在候选快照发布前编译；语法、常量或能力校验失败时拒绝整个候选并保留旧快照                    |
| ROUTE-JS-005 | P0     | 每个命中请求使用请求级 heap/context 执行已编译程序，不在热路径重新解析或编译                        |
| ROUTE-JS-006 | P0     | 脚本可同步读取请求，异步读取/丢弃 body，并通过 `resp.*` 发送响应                                    |
| ROUTE-JS-007 | P0     | 脚本未显式发送响应时：返回值生成 200 JSON，正常无返回生成 204                                       |
| ROUTE-JS-008 | P0     | 未提交响应前发生异常/abort 时生成稳定的 500 脚本执行错误；响应已提交后不尝试二次响应                |
| ROUTE-JS-009 | P0     | 脚本 Route 仍受 Host/entry/Project CIDR/HTTPS 策略约束，并继续产生有界 metrics、trace 和 access log |
| ROUTE-JS-010 | P0     | 脚本源码、请求/响应 body、敏感 header 不写入日志、错误响应或 validator 输出                         |
| ROUTE-JS-011 | P0     | `gzip` 可选为 boolean 或 1-9 级别；最终响应仅对固定常见 MIME、状态和长度条件执行转换                |

当前请求脚本读取 API 不能对未知长度流实施宿主级累计上限，因此 JS Route 只接受无 body，或
Content-Length 已知且不超过 server 全局 request body limit 的请求；chunked/stream body 以 413
fail closed。后续只有在 Fiber 公共 API 提供有界脚本读取后才能放宽。

### 4.3 Console 与版本模型

| 编号              | 优先级 | 需求                                                                                                   |
| ----------------- | ------ | ------------------------------------------------------------------------------------------------------ |
| ROUTE-CONSOLE-001 | P0     | Route 条目必须有显式 `format: yaml \| js` 判别字段                                                     |
| ROUTE-CONSOLE-002 | P0     | YAML 条目只保存 `id`、`format`、`source`；path/method 从 YAML 正文解析                                 |
| ROUTE-CONSOLE-003 | P0     | JS 条目保存 `id`、`format`、`source`、`path`、可选 `method` 和可选 `gzip`                              |
| ROUTE-CONSOLE-004 | P0     | 新建普通 Route 默认仍为 YAML；界面另提供新建 JS Route                                                  |
| ROUTE-CONSOLE-005 | P0     | 复制、排序、删除、历史版本预览、恢复、乐观锁和不可变版本语义对两种格式一致                             |
| ROUTE-CONSOLE-006 | P0     | 保存前进行格式相关本地校验；完整校验仍以 Native Validator 为准                                         |
| ROUTE-CONSOLE-007 | P0     | Project 网络策略为权威来源时，编译器将 `allows` 注入两种 wire Route；JS 编辑器不暴露 Route 级 `allows` |
| ROUTE-CONSOLE-008 | P0     | 旧 schema v1-v4 读取时确定性升级为 YAML 条目，不改变发布语义                                           |

## 5. Wire 契约

旧 YAML Route 的 JSON 对象保持原结构，仅新增可选字段：

```json
{
  "path": "/orders/:id",
  "method": "GET",
  "type": "PROXY",
  "service": "orders/stable"
}
```

JavaScript Route 使用同一个有序 `routes` 数组，格式为：

```json
{
  "path": "/diagnostics/:id",
  "method": "POST",
  "gzip": true,
  "type": "SCRIPT",
  "script": "let body = req.readJson(); return {id: $path.id, body: body};"
}
```

- `method` 缺失或 `null` 表示全部 method；
- `gzip` 缺失或 `false` 表示 identity；`true` 使用级别 6，整数 `1..9` 指定级别；
- `type: SCRIPT` 时 `script` 必须非空；
- `type: PROXY|RESPONSE` 时 `script` 必须缺失；
- 未识别 type、非法组合或脚本编译失败均拒绝完整候选；
- 项目列表 Data ID、group 和 per-project Data ID 不变。

## 6. 顺序与冲突示例

以下配置有效，请求按列表顺序尝试：

1. `GET /items/:id` JavaScript；
2. `POST /items/:id` YAML RESPONSE；
3. all-method `/items/:id` YAML PROXY fallback。

以下配置无效：

1. all-method、无 condition 的 `/items/:id`；
2. `GET /items/:id`。

第二条永远不可达，因此候选编译必须返回 dead-route conflict。

## 7. 验收标准

- 旧 fixture 和无 method 测试保持通过；
- codec 覆盖 method/SCRIPT 的成功、null、非法字段和非法组合；
- matcher 覆盖同 path 不同 method、all-method fallback、method+condition、dead route；
- handler 覆盖 JS 显式响应、返回值、无返回、编译失败、执行失败和 path 常量；
- Console 编译器覆盖 YAML method、混合列表、JS 外置字段和确定性 wire；
- Console/native 覆盖 gzip 类型、Accept-Encoding 协商、固定 MIME、静态预压缩和动态流式响应；
- schema 升级、API 校验、编辑器交互和历史预览覆盖两种格式；
- native configure/build/CTest 与 Console typecheck/test/format/build 全部通过；
- 文档继续声明生产脚本 corpus differential 与最终 cutover gate 未完成。
