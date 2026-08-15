# Access Gateway 脚本路由用法

脚本路由用于在 access-server 请求期读取请求数据、执行轻量计算并直接生成响应。它不是 Node.js、浏览器
JavaScript，也不是通用边缘函数平台；脚本运行在 Fiber 的 JS-like 字节码解释器中，并受固定函数集、
请求体和生命周期边界约束。

> 选择原则：固定内容用 `RESPONSE`，普通 HTTP/WebSocket 转发用 `PROXY`，确实需要请求期分支、JSON
> 组装、编码或哈希时才用 JavaScript Route。当前脚本路由不能发起出站 HTTP 请求。

## 1. 在 Console 中创建

1. 打开 Project 的 **Routes** 页面；
2. 点击 **+ JS**；
3. 在脚本卡片上方填写外置 `Path pattern`；
4. 可选填写 `Method`，留空表示所有 HTTP method；
5. 在 JavaScript 编辑器中填写脚本正文；
6. 修复浏览器本地问题后保存为新的不可变配置版本；
7. 由 server 编译为 wire Route，再交给 Native Validator 使用与 access-server 相同的 codec/compiler
   校验；
8. 创建并执行 Release 后，分别观察 Published 和实例 Active 状态。没有实例证据时 Active 仍为未知。

JavaScript Route 的 Console 模型大致为：

```typescript
interface JavaScriptRouteItem {
    id: string;
    format: "js";
    path: string;
    method?: string;
    source: string;
}
```

发布时确定性编译为 rnacos wire 对象：

```json
{
    "path": "/diagnostics/:id",
    "method": "POST",
    "type": "SCRIPT",
    "script": "let body = req.readJson(); return {id: $path.id, body};"
}
```

不要在正文中声明 `path`、`method` 或 `type`；正文不是路由元数据来源。

## 2. 最小示例

外置 Path：`/diagnostics/:id`

外置 Method：`GET`

脚本：

```javascript
return {
    id: $path.id,
    method: $req.method,
    path: $req.path,
    source: $query.source || "unknown"
};
```

脚本没有显式发送响应，因此返回 object 会自动产生：

```http
HTTP/1.1 200 OK
Content-Type: application/json

{"id":"...","method":"GET","path":"/diagnostics/...","source":"..."}
```

## 3. 匹配规则

脚本执行前仍会经过完整的 data-plane 策略：

1. Host 选择 Project；
2. Host 级 `X-Entry` 与 HTTPS 重定向；
3. Path matcher；
4. 可选 method 精确匹配；
5. Project/Route CIDR 和请求体限制；
6. 执行已经预编译的脚本。

Path 支持静态段、`:name` 参数段和末尾 `*name` wildcard。`$path.name` 只能引用外置 Path 已声明的
变量，拼写错误会在 Native Validator 阶段拒绝整个候选。

Method 留空匹配所有 method；填写后按原始字节大小写敏感匹配。脚本 Route 不支持 YAML
`condition`，也不能在开始执行后“放弃并尝试下一条 Route”。如果需要按 method 分流，应创建多个同
Path Route；如果需要 condition fallback，使用 YAML Route 的 condition，或让脚本自身生成完整响应。

## 4. 响应规则

### 4.1 隐式响应

脚本没有调用 `resp.send*()` 时，根据最终执行结果生成响应：

| 脚本结果                         | HTTP 行为                                  |
| -------------------------------- | ------------------------------------------ |
| `return value;`                  | 200，value JSON 编码                       |
| `return null;`                   | 200，body 为 JSON `null`                   |
| `return undefined;`              | 200，body 为 JSON `null`                   |
| `return;` 或执行到结尾           | 204，空 body                               |
| 未捕获 Exception / runtime Abort | 500 `SCRIPT_EXECUTION`，前提是响应尚未提交 |

为了让状态码和 content type 一目了然，业务 API 通常应使用显式响应。

### 4.2 显式 JSON 响应

```javascript
let page = req.getQuery("page") || "1";

resp.setHeader("Cache-Control", "no-store");
resp.sendJson(200, {
    ok: true,
    page,
    requestPath: req.getPath()
});

return;
```

`resp.sendJson(status, body)` 设置 `Content-Type: application/json` 并结束响应。

### 4.3 文本、二进制和空响应

```javascript
// 文本：自动设置 text/plain;charset=utf-8
resp.send(200, "ready");
return;
```

```javascript
// 二进制：原样写出，不自动设置 Content-Type
let bytes = binary.fromHex("89504e47");
resp.setHeader("Content-Type", "application/octet-stream");
resp.send(200, bytes);
return;
```

```javascript
// 空响应
resp.send(204);
return;
```

发送函数是异步宿主函数，但脚本语法不写 `await`；VM 会在普通函数调用位置暂停并恢复。响应发送后应
立即 `return;`，不要再次修改 header、再次发送或依赖发送后的异常来改变已经提交的 HTTP 结果。

## 5. 读取请求

### 5.1 Metadata

```javascript
let request = {
    method: req.getMethod(),
    uri: req.getUri(),
    path: req.getPath(),
    rawQuery: req.getQueryStr(),
    contentType: req.getHeader("Content-Type"),
    query: req.getQuery(),
    cookies: req.getCookie()
};

return request;
```

常量形式避免构造完整 header/query/cookie object，适合读取已知字段：

```javascript
return {
    id: $path.id,
    mode: $query.mode,
    userAgent: $header.user_agent,
    session: $cookie.session,
    traceCluster: $context.hi_trace_cluster,
    remoteAddress: $conn.remote_addr,
    tls: $conn.tls
};
```

`$header`、`$cookie`、`$context` 的名称执行 ASCII 小写折叠和 `-`/`_` 归一化。`$query` 的特殊 key
若不能写成标识符，请改用 `req.getQuery("key.with.dot")`。

### 5.2 JSON body

外置 Method：`POST`

```javascript
try {
    let input = req.readJson();
    let name = input.name;

    if (typeof name !== "string" || length(name) == 0) {
        resp.sendJson(400, { error: "name is required" });
        return;
    }

    resp.sendJson(201, {
        name,
        normalized: strings.toLower(strings.trim(name))
    });
    return;
} catch (error) {
    resp.sendJson(400, { error: "invalid JSON body" });
    return;
}
```

`req.readJson()` 读取完整 body。空 body、读取错误或非法 JSON 抛出可捕获 `Error`。

### 5.3 Binary body

```javascript
let payload = req.readBinary();
return {
    bytes: length(payload),
    sha256: hash.sha256(payload),
    base64: binary.base64Encode(payload)
};
```

空 body 返回长度为 0 的 `Binary`。`readBinary()` 同样会把完整 body 连续化到请求内存中。

### 5.4 丢弃 body

```javascript
req.discardBody();
resp.send(204);
return;
```

请求体是一次性流。`readJson()`、`readBinary()` 和 `discardBody()` 不应组合调用或重复调用。

## 6. 请求体边界

脚本读取函数本身会缓冲完整 body。为避免未知长度请求造成无界内存，access-server 在进入 SCRIPT 前
执行 fail-closed 规则：

- 无 body：允许；
- 有合法 Content-Length 且不超过 server 全局 request body limit：允许；
- Content-Length 超限：413 `REQ_BODY_TOO_LARGE`；
- chunked 或其他无法预先确定长度的 body：413 `REQ_BODY_TOO_LARGE`。

当前 SCRIPT 不接受 Route 级 `max_client_body_size` 字段。Console 的 JavaScript editor 也不在正文中
暴露这个字段。需要大 body 或端到端流式转发时使用 `PROXY`；不要用脚本读取后再模拟代理。

## 7. Header 与 Cookie

### 7.1 设置响应 header

```javascript
resp.setHeader("Content-Language", "zh-CN");
resp.addHeader("Vary", "Accept-Language");
resp.addHeader("Vary", "Accept-Encoding");
resp.sendJson(200, { ok: true });
return;
```

`setHeader` 替换同名待发送值，`addHeader` 追加。名称或转换后的 value 为空会抛 `Error`。协议 framing
仍由 `HttpExchange` 控制；不要设置 `Content-Length`、`Transfer-Encoding`、`Connection` 等
hop-by-hop/framing header。

Host 命中后由 access-server 准备的 HSTS 和 trace response header 会先复制到脚本响应上下文。脚本
可以设置普通业务 header，但不得回显 Authorization、cookie、内部 trace secret 或请求/响应 body。

### 7.2 添加 Cookie

```javascript
let added = resp.addCookie({
    name: "locale",
    value: "zh-CN",
    path: "/",
    maxAge: 3600,
    secure: true,
    httpOnly: true,
    sameSite: "Lax"
});

if (!added) {
    resp.sendJson(500, { error: "cookie configuration is invalid" });
    return;
}

resp.send(204);
return;
```

`name` 必填，`maxAge` 必须是整数，布尔字段必须是 boolean，`sameSite` 只接受大小写敏感的 `Lax`、
`Strict` 或 `None`。无效对象返回 `false`，不抛出详细字段错误。

## 8. 错误处理

脚本可以捕获标准库和 HTTP 库产生的语义异常：

```javascript
try {
    let token = req.getHeader("X-Payload");
    let decoded = binary.base64Decode(token);
    return { payload: JSON.parse(strings.toString(decoded)) };
} catch (error) {
    resp.sendJson(400, { error: "invalid payload" });
    return;
}
```

建议给 client 返回稳定、非敏感的错误结构，不要返回原始 `error` object、脚本源码、header、cookie 或
body。未捕获异常和不可捕获的 runtime Abort 在响应未提交时映射为稳定 500 `SCRIPT_EXECUTION`。

配置期错误与请求期错误必须区分：

| 阶段             | 示例                                       | 结果                            |
| ---------------- | ------------------------------------------ | ------------------------------- |
| Console 本地校验 | path 空、method 非 HTTP token、source 为空 | 阻止保存                        |
| Server 编译      | model shape、大小、wire 编译错误           | 阻止版本/Release                |
| Native Validator | 语法错误、未知函数、`$path` 名称不存在     | 拒绝候选，不替换旧快照          |
| 请求执行         | JSON body 非法、函数类型错误、显式 `throw` | 可由 `try/catch` 处理，否则 500 |
| Runtime Abort    | OOM、InvalidState 等                       | 不可捕获；终止本次执行          |

Native Validator 的错误应包含稳定 code、Route field path 和有界编译位置，不应回显完整脚本源码。

## 9. 当前可用和不可用能力

### 9.1 可用

- JS-like 字面量、变量、对象、数组、运算符和控制流；
- 标准库：array、strings、binary、hash、math、rand、JSON、Object、URL；
- `req.*` request metadata/body；
- `resp.*` header/cookie/response；
- `$path`、`$query`、`$header`、`$cookie`、`$context`、`$req`、`$conn`；
- 同步和由宿主注册的异步函数；异步调用不写 `await`；
- 配置期编译、请求期执行不可变程序。

### 9.2 不可用

- Node.js、浏览器或 ECMAScript 完整运行时；
- `fetch`、`XMLHttpRequest`、文件系统、环境变量、socket、timer、module/import；
- Promise、`async`/`await`、用户自定义 function/class；
- `while`、传统三段式 `for`、正则表达式；
- 动态对象方法，例如 `items.push(x)`；应写 `array.push(items, x)`；
- 出站 `directive backend = http "..."`、`http.request()`、`proxyPass()` 或任何动态 upstream API；
- 在 Console、server 或 Native Validator 中执行真实请求脚本；只有 access-server data plane 执行。

虽然固定 Fiber 库包含可供其他宿主启用的 HTTP upstream directive，Access Gateway 的
`AccessScriptRuntime` 明确以 `http_directives_enabled = false` 编译 Route。需要 upstream 时创建
YAML `PROXY` Route，不能在脚本中绕过 service discovery、header 保护、body limit 和观测策略。

## 10. 常用配方

### 10.1 健康和诊断信息

外置 Path：`/_gateway/diagnostics`

外置 Method：`GET`

```javascript
resp.setHeader("Cache-Control", "no-store");
resp.sendJson(200, {
    method: $req.method,
    path: $req.path,
    httpVersion: $conn.http_version,
    scheme: $conn.scheme,
    tls: $conn.tls
});
return;
```

不要在公开诊断端点返回 remote address、内部 header 或 trace context，除非已经完成认证授权和字段审计。

### 10.2 稳定 canary 分桶

```javascript
let user = req.getCookie("user_id") || req.getHeader("X-User-Id") || "anonymous";
let canary = rand.canary(5, user);

return {
    bucket: canary ? "canary" : "stable"
};
```

带 key 的 `rand.canary()` 使用稳定 CRC-32 分桶。它只适合流量选择，不用于密码学、安全 token 或访问
控制。脚本不能据此动态代理到不同 upstream；实际流量灰度仍应使用 data-plane 的 cluster/gray 策略。

### 10.3 表单 query 规范化

```javascript
let query = req.getQueryStr();

try {
    let parsed = URL.parseQuery(query);
    parsed.page = parsed.page || "1";
    return {
        parsed,
        canonical: URL.buildQuery(parsed)
    };
} catch (error) {
    resp.sendJson(400, { error: "invalid query encoding" });
    return;
}
```

`URL.*` 使用 `application/x-www-form-urlencoded` 语义：空格编码成 `+`，`+` 解码为空格。

### 10.4 内容摘要

```javascript
let body = req.readBinary();
resp.sendJson(200, {
    length: length(body),
    sha256: hash.sha256(body)
});
return;
```

只对全局 body limit 内的小请求使用。MD5/SHA-1 仅用于互操作，安全摘要优先使用 SHA-256。

## 11. 性能与安全建议

1. 在 Path/method 层先缩小请求范围，避免让所有流量进入脚本；
2. 优先读取 `$header.x`、`$query.x` 等单值常量，只有确实需要枚举时才构造完整 object；
3. 不重复 JSON stringify/parse，不对大 array/object 做多轮复制和展开；
4. body 会完整缓冲，保持 payload 小且显式要求 Content-Length；
5. 显式发送后立即 return；所有可预期的输入错误使用 try/catch 转成稳定 4xx；
6. 不把任意 Project、Path、header 或用户输入变成 metrics label；
7. 不记录脚本源码、Authorization、cookie、header secret 或 body；
8. 对输出 JSON 使用 `resp.sendJson()`，对 URL/form 使用 `URL.*`，不要手工拼接不可信输入；
9. 不把 hash/canary 当作认证、授权或密码学随机；
10. 发布前运行 Native Validator，并保留失败时旧快照继续服务的预期；发布后仍需实例激活证据。

完整的语言、标准库和 HTTP 函数签名见“脚本语法、标准库与 HTTP API 参考”。
