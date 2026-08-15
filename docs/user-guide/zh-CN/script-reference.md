# Access Gateway 脚本语法、标准库与 HTTP API 参考

本文是 Access Gateway Route condition、template 和 JavaScript Route 使用的完整脚本参考。文中的
TypeScript 只用于描述参数与返回值；运行时不是 TypeScript/JavaScript 引擎，也不读取 `.d.ts` 文件。

实现来源是当前仓库的 `native/access-server/src/runtime/AccessScriptRuntime.cpp`，以及固定 Fiber revision
`0fda7764bf94944aca4b674ab5ab311184703118` 中的 `script` 和 `http_script` 模块。

## 1. 运行时概览

Fiber Script 是面向网关配置的轻量 JS-like 字节码解释器：

- 配置加载阶段完成 tokenize、parse、字节码编译、函数解析和常量校验；
- 只有完整 Project 候选全部成功，才发布新的不可变快照；
- 请求阶段复用编译产物，每次请求使用独立 heap 和 `ScriptExchangeCtx`；
- 同步和异步宿主函数在编译期已知；脚本调用异步函数时仍不写 `await`；
- 未知函数、参数数量不匹配、未知常量或不允许的能力都是配置期错误。

```javascript
// readJson 和 sendJson 是异步宿主函数，但语法中没有 await。
let body = req.readJson();
resp.sendJson(200, body);
return;
```

Access Gateway 有三种脚本上下文：

| 上下文            | 允许赋值/控制流      | 异步函数           | `req.*`         | `resp.*` | 请求常量 |
| ----------------- | -------------------- | ------------------ | --------------- | -------- | -------- |
| YAML `condition`  | 否；输入是一条表达式 | 拒绝               | 仅同步 metadata | 否       | 是       |
| Template `${...}` | 否；每段是一条表达式 | 拒绝               | 仅同步 metadata | 否       | 是       |
| JavaScript Route  | 是；完整脚本         | 允许已注册异步函数 | 是              | 是       | 是       |

所有 Access Gateway 上下文都禁用出站 HTTP directive。

## 2. 数据类型

可用下列近似类型描述脚本值：

```typescript
declare class Binary {
    private readonly __scriptBinaryBrand: never;
}

type ScriptPrimitive = undefined | null | boolean | number | string | Binary;
type ScriptArray = ScriptValue[];
type ScriptObject = { [key: string]: ScriptValue };
type ScriptValue = ScriptPrimitive | ScriptArray | ScriptObject;
```

### 2.1 类型规则

- 数字内部区分 signed 64-bit integer 和 `double`，脚本层统一表现为 `number`；
- `undefined` 表示缺失属性、未初始化变量或 API 的“无值”，与 `null` 不同；
- 字符串使用 WTF-8/UTF-16 语义，`length()`、字符串下标和 `strings.substring()` 的位置单位是
  UTF-16 code unit；例如 `length("😀") == 2`；
- `Binary` 保存原始 byte，通常来自 `req.readBinary()`、`binary.base64Decode()`、
  `binary.fromHex()` 或宿主；
- array 和 object 是可变引用值；`array.push/pop`、`Object.assign/deleteProperties` 原地修改；
- object 属性保持插入顺序；相关迭代和 JSON 输出遵循该顺序；
- Route script 的根值 `$` 由 Access Gateway 以 `undefined` 传入。不要用 `$.field`；请求数据应从
  `$req`、`$path` 或 `req.*` 读取。

`typeof` 返回本运行时的类型名：

```text
undefined, null, boolean, number, string, binary,
array, object, iterator, exception
```

## 3. 词法和基本语法

### 3.1 注释与字面量

支持 `//` 行注释和 `/* ... */` 块注释，以及以下字面量：

```javascript
let integer = 42;
let floating = 0.25;
let enabled = true;
let empty = null;
let missing = undefined;
let text = "gateway";
let anotherText = "route";
let list = [1, 2, integer];
let object = { name: text, enabled: enabled };
```

未闭合字符串、注释或 template literal 是配置期 parse error。

### 3.2 变量、object 和 array

只使用 `let` 声明变量：

```javascript
let name = "fiber";
let base = { a: 1 };
let object = { name, ...base, ["dynamic-key"]: 2 };
let list = [0, ...[1, 2], 3];

let first = list[0];
let current = object.name;
object.name = "gateway";
object["new-key"] = 2;
```

支持 object 属性简写、计算属性和 spread，array 也支持 spread。object 字面量中重复的静态 key 是
编译错误。array 下标赋值只能修改已有下标；追加请用 `array.push(list, value)`。

`let` 声明必须以分号结束。为避免解析歧义，所有 statement 都应写分号。

### 3.3 运算符

```text
一元：+  -  !  typeof
算术：+  -  *  /  %
比较：<  <=  >  >=  ==  !=  ===  !==
逻辑：&&  ||
成员：in
条件：condition ? whenTrue : whenFalse
赋值：=
```

- `&&`、`||` 短路并返回参与运算的值，可用于默认值；
- 任一操作数为 string 时，`+` 执行字符串拼接；object、array 和 binary 不能隐式拼接；
- 数值运算接受 number、boolean 和 `null`，不会像普通 JavaScript 一样把任意 string 隐式转成 number；
- `==`/`!=` 是宽松比较，`===`/`!==` 是严格比较；array/object 的严格比较使用引用身份；
- `key in object` 判断属性是否存在，`index in array` 判断整数下标是否在范围内，其他组合为 false；
- `~` match 运算符能被 parser 识别，但当前 bytecode compiler 会拒绝，正则能力未开放；
- 不支持 `++`、`--`、复合赋值、可选链或 nullish coalescing；使用显式表达式。

示例：

```javascript
let role = $header.x_role || "guest";
let allowed = role === "admin" || role === "operator";
return allowed ? { ok: true } : { ok: false };
```

### 3.4 控制流

支持 `if/else`、双变量 `for-of`、`break`、`continue`、`return`、`try/catch` 和 `throw`：

```javascript
let result = [];

if (req.getMethod() === "GET") {
    for (let index, value of [1, 2, 3]) {
        if (value == 2) {
            continue;
        }
        array.push(result, value);
    }
} else {
    return [];
}

return result;
```

`for-of` 只支持以下形式：

```javascript
// array: key 是从 0 开始的下标，value 是元素。
for (let key, value of arrayValue) {
    // ...
}

// object: key 是属性名，value 是属性值，按插入顺序。
for (let key, value of objectValue) {
    // ...
}
```

迭代期间增加/删除 array 元素或 object 属性属于结构修改，会产生可捕获 `IterationError`；更新已有
object 属性的 value 不属于结构修改。

不支持 `while`、`do/while`、传统三段式 `for`、`switch`、function/class 声明、arrow function、
generator、module 或 Promise。

### 3.5 异常

```javascript
try {
    let value = JSON.parse(req.getHeader("X-JSON"));
    return { ok: true, value };
} catch (error) {
    return { ok: false };
}
```

`throw value;` 可以抛出任意 ScriptValue。类型、范围、JSON/URL parse 和 HTTP 函数错误也可被
`try/catch` 捕获。

运行结果区分：

- Exception：脚本语义错误，可被 catch；未捕获时 Route 返回 500；
- Abort：运行时终止，例如 OutOfMemory、InvalidState、Timeout，脚本不能捕获。

不要把捕获到的原始 error 直接返回客户端，它可能暴露实现细节。

### 3.6 Template literal

脚本正文支持反引号 template literal：

```javascript
let id = $path.id;
return `item-${id}-${1 + 2}`;
```

YAML Route 的 template 是另一层配置语法：正文不带反引号，使用 `${expression}`：

```yaml
rewrite: "/internal/${$path.id}"
response_headers:
    X-Route: "item-${$path.id}"
```

两者都在配置期编译。YAML template expression 禁止赋值和异步函数。

### 3.7 函数调用与参数

函数名在编译期静态解析：

```javascript
array.push(items, 1, 2);
array.push(items, ...moreItems);
Object.assign(target, ...sources);
```

未知函数、固定参数数量不匹配、spread 与签名不兼容或重载歧义会导致配置编译失败。没有动态 prototype
method；`items.push(x)` 不可用，必须写 `array.push(items, x)`。

## 4. 标准库总览

下列函数在 condition、template 和 JavaScript Route 中均注册；condition/template 只能调用同步函数。

```text
length                         includes
array.join                     array.pop                       array.push
strings.hasPrefix              strings.hasSuffix               strings.toLower
strings.toUpper                strings.trim                    strings.trimLeft
strings.trimRight              strings.split                   strings.contains
strings.contains_any           strings.index                   strings.indexAny
strings.lastIndex              strings.lastIndexAny            strings.repeat
strings.substring              strings.toString
binary.base64Encode            binary.base64Decode             binary.hex
binary.fromHex                 binary.getUtf8Bytes
hash.crc32                     hash.md5                         hash.sha1
hash.sha256
math.floor                     math.abs
rand.random                    rand.canary
JSON.parse                     JSON.stringify
Object.assign                  Object.keys                     Object.values
Object.deleteProperties
URL.encodeComponent            URL.decodeComponent             URL.parseQuery
URL.buildQuery
```

## 5. 通用函数

```typescript
function length(value?: ScriptValue): number;
function includes(container: string | ScriptValue[], ...items: ScriptValue[]): boolean;
```

### `length(value)`

- 省略参数等同于 `null`；
- string 返回 UTF-16 code unit 数；
- Binary 返回 byte 数；array 返回元素数；object 返回属性数；
- `null`、`undefined`、number、boolean 和其他类型返回 0。

### `includes(container, ...items)`

- string container 要求每个 item 都是它的 substring；
- array container 要求每个 item 都能以严格相等 `===` 找到；
- 其他 container 返回 false；
- 没有 item 时，合法 string/array container 返回 true。

## 6. Array 函数

```typescript
namespace array {
    function join(values: ScriptValue[], separator?: ScriptValue): string;
    function pop(values: ScriptValue[]): ScriptValue | null;
    function push(values: ScriptValue[], ...items: ScriptValue[]): ScriptValue[];
}
```

- `array.join()` 默认 separator 是空字符串，不是 JavaScript 的逗号。string、number、boolean 转为
  text；`null`、`undefined`、container 和 Binary 作为空文本；非 array 抛 TypeError；
- `array.pop()` 原地删除并返回末项，空 array 返回 `null`，非 array 抛 TypeError；
- `array.push()` 原地追加并返回 array 自身，不返回新 length，非 array 抛 TypeError。

## 7. String 函数

```typescript
namespace strings {
    function hasPrefix(text: string, prefix: string): boolean;
    function hasSuffix(text: string, suffix: string): boolean;
    function toLower(text: string): string | null;
    function toUpper(text: string): string | null;

    function trim(text: string, cutset?: string | null): string | null;
    function trimLeft(text: string, cutset?: string | null): string | null;
    function trimRight(text: string, cutset?: string | null): string | null;
    function split(text: string, separators?: string | null): string[] | null;

    function contains(text: string, value: string): boolean | null;
    function contains_any(text: string, chars: string): boolean | null;
    function index(text: string, value: string): number | null;
    function indexAny(text: string, chars: string): number | null;
    function lastIndex(text: string, value: string): number | null;
    function lastIndexAny(text: string, chars: string): number | null;

    function repeat(text: string, count: number): string | null;
    function substring(text: string, start?: number, end?: number): string | null;

    function toString(): string;
    function toString(value: ScriptValue): string;
}
```

共同规则：需要 string 的参数类型错误时，多数函数按兼容语义返回 `null`；`hasPrefix()` 和
`hasSuffix()` 返回 false，而不是抛异常。

- `toLower()`/`toUpper()` 只转换 ASCII `A-Z`/`a-z`，非 ASCII byte 保持不变；
- `trim(text)` 删除两端 byte value `<= 0x20` 的字符；
- 传入 `cutset` 时，trim 函数把它当作一个完整 substring 从对应端反复删除，不是字符集合；
- `trimLeft/trimRight` 未传 cutset 时删除当前实现支持的 ASCII Java whitespace；
- `split(text)` 未传 separator 返回 `[text]`；传入后把 separator 当作 Unicode code point 集合，
  任一字符都可分隔，连续和尾部分隔符不生成空项；
- `contains_any/indexAny` 把 `chars` 当作 code point 集合；
- `index/indexAny/lastIndex` 返回 UTF-16 index，找不到返回 -1；
- `lastIndexAny` 保留兼容行为：按 `chars` 的字符顺序，返回第一个有匹配候选字符的最后位置，不是所有
  候选位置的全局最大值；
- `repeat` 的浮点 count 向 0 截断，负值或非数值返回 null；单次结果限制 16 MiB，超过会 Abort；
- `substring` 的 start/end 是 UTF-16 index，默认 0 和 `2147483647`；负 start 归零，`end <= start`
  返回空字符串；
- `toString()` 无参数返回空字符串；`null`/`undefined` 返回 `"null"`；object/array 返回内部兼容文本
  `<ObjectNode>`/`<ArrayNode>`，不是 JSON。需要 JSON 时使用 `JSON.stringify()`。

`strings.match` 和 `strings.findAll` 未注册。

## 8. Binary 函数

```typescript
namespace binary {
    function base64Encode(value: Binary): string | undefined;
    function base64Decode(value: string): Binary | undefined;
    function hex(value: Binary): string;
    function fromHex(value: string): Binary;
    function getUtf8Bytes(value: ScriptValue): Binary;
}
```

- `base64Encode` 只接受 Binary，其他类型返回 undefined；
- `base64Decode` 只接受 string，其他类型返回 undefined；非法字符、padding、非 4 倍数长度或 whitespace
  抛 RangeError；
- `hex` 返回小写 hex，非 Binary 抛 TypeError；
- `fromHex` 严格要求偶数长度和合法 hex，非法内容抛 RangeError，非 string 抛 TypeError；
- `getUtf8Bytes` 把兼容文本表示编码为 UTF-8。object/array 分别编码 `<ObjectNode>` 和
  `<ArrayNode>`，它不是 JSON-to-bytes；需要 JSON byte 时先 `JSON.stringify()`，再传给该函数；
- 当前没有通用的 Binary-to-UTF-8-string decoder。需要解析 JSON body 时直接使用 `req.readJson()`。

## 9. Hash 函数

```typescript
namespace hash {
    function crc32(value: ScriptValue): number;
    function md5(value: string | Binary): string;
    function sha1(value: string | Binary): string;
    function sha256(value: string | Binary): string;
}
```

- `crc32` 对兼容文本表示计算 CRC-32，返回 `0..0xffffffff`；container 和 Binary 的兼容文本为空；
- md5/sha1/sha256 接受 UTF-8 string 或原始 Binary，返回小写 hex，其他类型抛 TypeError；
- MD5 和 SHA-1 只用于旧系统互操作，不用于签名、密码或任何要求抗碰撞性的安全设计。

## 10. Math 与随机函数

```typescript
namespace math {
    function floor(value: number): number;
    function abs(value: number): number;
}

namespace rand {
    function random(max?: number): number;
    function canary(ratio: number, ...keys: ScriptValue[]): boolean;
}
```

- `math.floor` 对 integer 原样返回，对 floating 向负无穷取整；非 number 抛 TypeError；
- `math.abs` 返回绝对值；为 Java 兼容，`INT64_MIN` 保持原值；非 number 抛 TypeError；
- `rand.random(max)` 返回 `[0,max)` 均匀 integer，max 默认 1000，floating 向 0 截断；`max <= 0`
  抛 RangeError，非 number 抛 TypeError；
- `rand.canary(ratio, ...keys)`：ratio <= 0 恒 false，>= 100 恒 true；无 key 时随机分桶，有 key 时按
  参数顺序累计 CRC-32 并稳定映射到 `[0,100)`；非数值 ratio 按 0；
- `rand.*` 不是密码学随机源，也不能替代 data-plane 的 production gray/service selection。

## 11. JSON 函数

```typescript
namespace JSON {
    function parse(text: string): ScriptValue;
    function stringify(value: ScriptValue): string | undefined;
}
```

- `JSON.parse` 只接受 string；非法 JSON 抛带解析 message 和 byte offset 的 SyntaxError，非 string 抛
  TypeError；
- `JSON.stringify(undefined)` 返回 undefined；nested undefined 编码为 JSON null；
- 顶层 NaN 和正负 infinity 编码为文本 `"null"`；其他非法 number 或不可编码 value 抛 TypeError；
- Binary 编码为 Base64 JSON string；
- object property 按插入顺序输出；
- 发送 HTTP JSON 优先用 `resp.sendJson()`，避免手工设置 content type 和 body。

## 12. Object 函数

```typescript
namespace Object {
    function assign(
        target: ScriptObject,
        source: ScriptValue,
        ...sources: ScriptValue[]
    ): ScriptObject;

    function keys(value: ScriptObject): string[];
    function values(value: ScriptObject): ScriptValue[];

    function deleteProperties(
        target: ScriptObject,
        key: ScriptValue,
        ...keys: ScriptValue[]
    ): ScriptObject;
}
```

- `Object.assign` 原地 merge object source 并返回 target；非 object source 静默跳过，非 object target
  抛 TypeError；覆盖属性不改变插入位置；
- `Object.keys/values` 按插入顺序返回新 array，非 object 抛 TypeError；
- `Object.deleteProperties` 原地删除所有 string key 并返回 target；非 string 和不存在的 key 静默跳过，
  非 object target 抛 TypeError。

## 13. URL 表单函数

这些函数使用 `application/x-www-form-urlencoded`，不是 ECMAScript 全局
`encodeURIComponent`：空格编码为 `+`，`+` 解码为空格。

```typescript
namespace URL {
    function encodeComponent(value: string): string;
    function decodeComponent(value: string): string;

    function parseQuery(value: string): {
        [key: string]: string | string[];
    };

    function buildQuery(
        value?: null | undefined | { [key: string]: ScriptValue | ScriptValue[] }
    ): string | null | undefined;
}
```

- `encodeComponent` 保留 alphanumeric、`-`、`_`、`.`、`*`，space 变 `+`，其余 UTF-8 byte 用大写
  `%HH`；非 string 抛 TypeError；
- `decodeComponent` 解码 `+` 和 `%HH`；非法 percent escape 抛 RangeError，非 string 抛 TypeError；
- `parseQuery` 返回 object；重复 key 从 string 提升为 array 并继续追加；无 `=` 的字段 value 为空
  string；空段跳过；
- `buildQuery` 按 object property 插入顺序生成 query；array 展开为重复 key，空 array 不生成字段；
  null/undefined 原样返回，其他非 object 抛 TypeError。

## 14. Request HTTP API：`req.*`

```typescript
declare namespace req {
    function getHeader(): { [name: string]: string };
    function getHeader(name: string): string | undefined | null;

    function getQuery(): { [name: string]: string };
    function getQuery(name: string): string | undefined;

    function getCookie(): { [name: string]: string };
    function getCookie(name: string): string | undefined;

    function getUri(): string;
    function getPath(): string;
    function getQueryStr(): string;
    function getMethod(): string;

    function readJson(): ScriptValue;
    function readBinary(): Binary;
    function discardBody(): null;
}
```

### 14.1 Metadata

- `getHeader()` 延迟构造并在本次执行缓存 request header object；同名字段折叠为一个 value；
- `getHeader(name)` 按 HTTP header 名称规则查找；不存在返回 undefined，空名称或非 string 返回 null；
- `getQuery()` 解析 raw query；重复 key 保留最后值；错误 percent escape 保留错误前已解析内容；
- `getQuery(name)` 不存在、空名称或非 string 返回 undefined；
- `getCookie()` 解析全部 Cookie header，同名 cookie 后值覆盖前值；
- `getCookie(name)` 不存在、空名称或非 string 返回 undefined；
- `getUri()` 返回 raw request target `path[?query]`，不含 scheme/host；
- `getPath()` 返回解析后 path；`getQueryStr()` 返回不含 `?` 的 raw query；
- `getMethod()` 返回原始 method token。

### 14.2 Body

- `readJson()` 完整读取并 parse JSON；空 body、读取失败或非法 JSON 抛可捕获 Error；
- `readBinary()` 完整读取 raw byte；空 body 返回 0-byte Binary；
- `discardBody()` 排空 body 并返回 null，底层 drain error 按兼容行为忽略；
- body 是一次性 stream，三个函数不能组合或重复读取；
- Access Gateway host 只允许无 body，或已知且在全局限制内的 Content-Length body 进入 SCRIPT；未知长度
  body 在函数调用前已经以 413 拒绝；
- condition/template 虽然能解析同步 `req.*`，但任何 body 函数都是 async，会导致配置编译失败。

## 15. Response HTTP API：`resp.*`

```typescript
type HeaderValue = string | number | boolean | null;

interface ResponseCookie {
    name: string;
    value?: ScriptValue;
    domain?: string;
    path?: string;
    maxAge?: number;
    secure?: boolean;
    httpOnly?: boolean;
    sameSite?: "Lax" | "Strict" | "None";
}

declare namespace resp {
    function setHeader(name: string, value: HeaderValue): null;
    function addHeader(name: string, value: HeaderValue): null;
    function addCookie(cookie: ResponseCookie): boolean;

    function sendJson(status: number, body: ScriptValue): null;
    function send(status: number): null;
    function send(status: number, body: ScriptValue): null;
}
```

### 15.1 Header 和 Cookie

- `setHeader` 替换同名 pending header；`addHeader` 追加；
- header name 或兼容文本化后的 value 为空抛 Error；header 已发出后静默不修改；
- `addCookie` 编码并追加 Set-Cookie；name 必填；maxAge 只接受 integer；secure/httpOnly 只接受 boolean；
  sameSite 大小写敏感；成功 true，无效 object/field false；
- 不要设置 hop-by-hop/framing header，不要输出 secret。

### 15.2 发送

- `sendJson(status, body)` JSON encode body，设置 `application/json` 并发送固定长度响应；status 非 integer
  回退 200；undefined 编码为 JSON null，Binary 编码为 Base64 JSON string；
- `send(status)` 发送空 body；
- `send(status, Binary)` 原样发送，不自动设置 content type；
- `send(status, string)` 以 UTF-8 发送并设置 `text/plain;charset=utf-8`；
- `send(status, other)` 走 JSON 并设置 `application/json`；
- send 函数提交 header 并结束 stream。正常脚本只调用一次，随后立即 `return;`。

## 16. 路由和连接常量

```typescript
declare const $path: { [routeVariable: string]: string | null };
declare const $query: { [queryName: string]: string | null };
declare const $header: { [normalizedHeaderName: string]: string | null };
declare const $cookie: { [normalizedCookieName: string]: string | null };
declare const $context: { [contextName: string]: string | null };

declare const $req: {
    uri: string;
    method: string;
    path: string;
    query: string;
};

declare const $conn: {
    remote_addr: string | null;
    remote_port: number;
    http_version: "HTTP/0.9" | "HTTP/1.0" | "HTTP/1.1" | "HTTP/2" | "HTTP/3";
    scheme: string;
    tls: boolean;
};
```

解析规则：

- `$path.name` 必须是当前 Path 的 capture name，否则配置编译失败；运行时未绑定返回 null；
- `$req` 只允许 `uri/method/path/query`，`$conn` 只允许上面五个字段，未知字段是配置错误；
- `$query.key` 和 `$context.key` 可为任意合法 script identifier，不存在返回 null；
- `$header/$cookie/$context` 名称 ASCII case-insensitive，`-` 与 `_` 归一化；
- dot 后必须是合法 identifier；特殊 key 使用 `req.getQuery/getCookie/getHeader`；
- 常量名称在 Project 配置编译期去重并分配 slot，请求期按 slot 绑定；读取已知单值通常比构造完整 object
  更轻量。

示例：

```javascript
return {
    id: $path.id,
    source: $query.source,
    forwardedFor: $header.x_forwarded_for,
    session: $cookie.session,
    traceCluster: $context.hi_trace_cluster,
    method: $req.method,
    remoteAddress: $conn.remote_addr,
    tls: $conn.tls
};
```

## 17. 出站 HTTP 能力

Access Gateway JavaScript Route **没有**以下 API：

```text
fetch(...)
http.request(...)
http.proxyPass(...)
directive backend = http "@service"
backend.request(...)
backend.proxyPass(...)
```

固定 Fiber 模块包含可由其他应用宿主启用的 HTTP directive 和 upstream client，但
`AccessScriptRuntime` 创建 compile scope 时明确传入 `http_directives_enabled = false`，也没有为请求
注入 outbound `HttpScriptServices`。因此这些名字会在配置编译期失败，而不是在运行时偷偷绕过策略。

需要 upstream 时使用 YAML `PROXY` Route。这样才能统一应用 NamingService、静态地址校验、header
保护、body limit、WebSocket、连接池、重试、trace、metrics、取消和 shutdown 语义。

## 18. 返回、错误和 HTTP 映射

JavaScript Route 未显式发送时：

| ScriptResult                 | 映射                   |
| ---------------------------- | ---------------------- |
| Value                        | 200 JSON               |
| Void（`return;` 或落到结尾） | 204 empty              |
| Exception / Abort            | 500 `SCRIPT_EXECUTION` |

若 `resp.send*` 已提交 header，宿主不会尝试第二次响应。发送后执行更多代码仍可能浪费资源或产生不可见
错误，因此必须立即 return。

Condition 的 Exception/Abort/non-value 不生成 500，而是让该候选 condition 视为 false。Template 的
失败则让所处 RESPONSE/PROXY 准备失败并进入稳定 500 错误路径。

配置错误会拒绝整个候选快照并保留上一成功快照；同 version 候选会被忽略。Draft 保存、rnacos
Published 和 instance Active 是不同状态，脚本成功通过 Native Validator 也不等于任何实例已经激活。

## 19. 快速选择表

| 目标                             | 推荐 API                                         |
| -------------------------------- | ------------------------------------------------ |
| 读取一个 Path capture            | `$path.id`                                       |
| 读取一个已知 query/header/cookie | `$query.x` / `$header.x` / `$cookie.x`           |
| 读取特殊字符 key                 | `req.getQuery("a.b")` 等                         |
| 枚举全部 query/header/cookie     | `req.getQuery()` / `getHeader()` / `getCookie()` |
| 读取 JSON                        | `req.readJson()`                                 |
| 读取 byte                        | `req.readBinary()`                               |
| 返回 JSON                        | `resp.sendJson(status, value)`                   |
| 返回 text                        | `resp.send(status, text)`                        |
| 返回 byte                        | `resp.send(status, binary)` 并显式 Content-Type  |
| JSON encode/decode string        | `JSON.stringify/parse`                           |
| Form query encode/decode         | `URL.buildQuery/parseQuery`                      |
| Base64                           | `binary.base64Encode/base64Decode`               |
| 安全摘要                         | `hash.sha256`                                    |
| 稳定百分比分桶                   | `rand.canary(ratio, key...)`，仅非安全用途       |
| 普通 upstream 转发               | 不用脚本；创建 YAML `PROXY` Route                |
