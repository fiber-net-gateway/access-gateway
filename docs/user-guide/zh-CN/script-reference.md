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

### 4.1 如何阅读函数说明

下文使用接近 TypeScript 的写法表达签名，但它只是文档记法，不表示脚本支持 TypeScript。`?` 或
`= default` 表示可省略参数，`...items` 表示可变参数。所有函数名和参数数量都在**配置编译期**解析：

- 少传必填参数、多传固定参数、调用不存在的函数或无法解析重载，会拒绝候选配置；这不是运行时异常，
  不能用 `try/catch` 捕获；
- 可选参数由运行时插入文档列出的默认值；可变参数可以为空，也可以使用 `...array` 展开；
- 参数类型通常在**执行期**检查。各函数的“异常”字段明确说明类型错误是返回 `null`/`undefined`/
  `false`，还是抛出可捕获异常；
- “返回原对象/原数组”表示原地修改且保持对象 identity，不会创建副本；
- string 的 index/length 除特别说明外使用 UTF-16 code unit，Binary 的 length 使用 byte。

### 4.2 异常与执行中止不是一回事

| 结果          | 能否 `catch` | JavaScript Route 的最终处理                                     |
| ------------- | ------------ | --------------------------------------------------------------- |
| `TypeError`   | 是           | 未捕获时映射为 500 `SCRIPT_EXECUTION`                           |
| `RangeError`  | 是           | 未捕获时映射为 500 `SCRIPT_EXECUTION`                           |
| `SyntaxError` | 是           | 未捕获时映射为 500 `SCRIPT_EXECUTION`                           |
| `Error`       | 是           | HTTP 读取、编码或写入失败等；未捕获时映射为 500                 |
| `Abort`       | 否           | 例如 `OutOfMemory`、`Timeout`、`Cancelled`、`InvalidState`；500 |
| 配置编译错误  | 否           | 候选快照被拒绝，旧快照继续服务                                  |

每个条目的“异常”默认只描述脚本可捕获异常。任何需要分配结果 string、Binary、array 或 object 的函数，
都可能在资源耗尽时以 `OutOfMemory` 中止；HTTP 异步函数还可能因宿主取消或超时中止。中止不是脚本语言的
`throw`，不会进入 `catch`。文档只在函数存在额外、稳定的中止条件时重复标注。

```javascript
let decoded;
try {
    decoded = binary.base64Decode("not-base64");
} catch (e) {
    // RangeError 会进入这里。
    decoded = binary.fromHex("");
}
```

## 5. 通用函数

```typescript
function length(value?: ScriptValue): number;
function includes(container: string | ScriptValue[], ...items: ScriptValue[]): boolean;
```

### `length(value = null)`

- 参数：`value` 可为任意脚本值；省略时默认 `null`。
- 返回：integer。string 返回 UTF-16 code unit 数；Binary 返回 byte 数；array 返回元素数；object
  返回自有属性数；`null`、`undefined`、number、boolean 及其他内部类型返回 0。
- 异常：不抛可捕获异常。
- 副作用：无。

```javascript
return {
    ascii: length("abc"), // 3
    emoji: length("A😀"), // 3：A 为 1，😀 为两个 UTF-16 code unit
    bytes: length(binary.fromHex("00ff10")), // 3
    fields: length({ a: 1, b: 2 }), // 2
    omitted: length() // 0
};
```

### `includes(container, ...items)`

- 参数 `container`：必填，只对 string 或 array 有匹配语义。
- 参数 `items`：零个或多个待检查值。
- 返回：boolean。string container 要求每个 item 都是 string 且都是它的 substring；array
  container 要求每个 item 都能用严格相等 `===` 找到。其他 container 或 string 中出现非 string item
  时返回 `false`。`items` 为空时，合法 string/array container 返回 `true`。
- 异常：不抛可捕获异常。
- 副作用：无。

```javascript
return {
    text: includes("access-gateway", "access", "way"), // true
    methods: includes(["GET", "POST"], req.getMethod()),
    strict: includes([1], "1"), // false：1 !== "1"
    emptyCheck: includes([]) // true
};
```

## 6. Array 函数

```typescript
namespace array {
    function join(values: ScriptValue[], separator?: ScriptValue): string;
    function pop(values: ScriptValue[]): ScriptValue | null;
    function push(values: ScriptValue[], ...items: ScriptValue[]): ScriptValue[];
}
```

### `array.join(values, separator = "")`

- 参数 `values`：必填，必须是 array。
- 参数 `separator`：可选，默认空字符串，不是 JavaScript 原生 `join()` 的逗号。string、integer、float
  和 boolean 转为文本；`null`、`undefined`、array、object 和 Binary 转为空文本。
- 返回：新 string。每个元素使用与 separator 相同的兼容文本转换。
- 异常：`values` 不是 array 时抛 `TypeError`。
- 副作用：不修改输入 array。

```javascript
return array.join(["route", 7, null, true], "|");
// "route|7||true"
```

### `array.pop(values)`

- 参数 `values`：必填，必须是 array。
- 返回：删除前的最后一个元素；空 array 返回 `null`。
- 异常：`values` 不是 array 时抛 `TypeError`。
- 副作用：原地删除一个元素；对空 array 不做修改。

```javascript
let values = ["a", "b"];
let last = array.pop(values);
return { last: last, remaining: values };
// {"last":"b","remaining":["a"]}
```

### `array.push(values, ...items)`

- 参数 `values`：必填，必须是 array。
- 参数 `items`：零个或多个任意脚本值，也可以通过 `...anotherArray` 展开。
- 返回：被修改的 `values` 本身，**不是**新长度。
- 异常：`values` 不是 array 时抛 `TypeError`。
- 副作用：按参数顺序把 `items` 原地追加到 `values`；没有 item 时保持不变。

```javascript
let values = [1];
let same = array.push(values, 2, ...[3, 4]);
return { values: values, sameObject: same === values };
// {"values":[1,2,3,4],"sameObject":true}
```

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

### `strings.hasPrefix(text, prefix)`

- 参数：`text` 和 `prefix` 都必须是 string；二者都必填。
- 返回：以 `prefix` 开头时为 boolean `true`，否则为 `false`。空 prefix 匹配任何合法 string。
- 异常：不抛；任一参数不是 string 时返回 `false`。

```javascript
return strings.hasPrefix("/api/v1/orders", "/api/"); // true
```

### `strings.hasSuffix(text, suffix)`

- 参数：`text` 和 `suffix` 都必须是 string；二者都必填。
- 返回：以 `suffix` 结尾时为 boolean `true`，否则为 `false`。空 suffix 匹配任何合法 string。
- 异常：不抛；任一参数不是 string 时返回 `false`。

```javascript
return strings.hasSuffix("archive.tar.gz", ".gz"); // true
```

### `strings.toLower(text)`

- 参数 `text`：必填 string。
- 返回：新 string，只把 ASCII `A-Z` 转为 `a-z`；UTF-8 多字节字符保持不变。非 string 返回
  `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.toLower("API-ÄBC-中文"); // "api-Äbc-中文"
```

### `strings.toUpper(text)`

- 参数 `text`：必填 string。
- 返回：新 string，只把 ASCII `a-z` 转为 `A-Z`；非 ASCII 字符保持不变。非 string 返回 `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.toUpper("api-äbc-中文"); // "API-äBC-中文"
```

### `strings.trim(text, cutset = null)`

- 参数 `text`：必填 string；非 string 返回 `null`。
- 参数 `cutset`：可选。省略、`null` 或其他非 string 值时，删除两端 byte value `<= 0x20` 的字符；
  传 string 时，把它当作一个**完整 substring**从左右两端反复删除。
- 返回：trim 后的新 string。空 cutset 不做任何删除。
- 异常：不抛可捕获异常。

```javascript
return {
    whitespace: strings.trim(" \t value \n"), // "value"
    marker: strings.trim("ababvalueabab", "ab") // "value"
};
```

### `strings.trimLeft(text, cutset = null)`

- 参数 `text`：必填 string；非 string 返回 `null`。
- 参数 `cutset`：省略或非 string 时，删除左侧 ASCII Java whitespace
  `0x09..0x0d`、`0x1c..0x20`；传 string 时，从左侧反复删除完整 substring。
- 返回：处理后的 string；不修改原值。
- 异常：不抛可捕获异常。

```javascript
return strings.trimLeft("///v1/items///", "/"); // "v1/items///"
```

### `strings.trimRight(text, cutset = null)`

- 参数与 `trimLeft()` 相同，但只处理右端。
- 返回：处理后的 string；空 cutset 不做删除，非 string `text` 返回 `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.trimRight("///v1/items///", "/"); // "///v1/items"
```

### `strings.split(text, separators = null)`

- 参数 `text`：必填 string；非 string 返回 `null`。
- 参数 `separators`：省略或非 string 时不拆分，返回只含原 string 的 array；传 string 时，其中每个
  Unicode code point 都是独立分隔符，不把整个字符串当作一个 delimiter。
- 返回：新 string array。连续分隔符会合并，前导/尾随分隔符不生成空元素；显式空 separators 不匹配
  任何字符，因此非空 text 返回 `[text]`，空 text 返回 `[]`。
- 异常：不抛可捕获异常。

```javascript
return {
    parts: strings.split("a,b;;c,", ",;"), // ["a", "b", "c"]
    untouched: strings.split("a,b") // ["a,b"]
};
```

### `strings.contains(text, value)`

- 参数：`text` 和 `value` 都必须是 string。
- 返回：找到完整 substring 时为 boolean `true`，找不到为 `false`；空 value 返回 `true`。任一参数
  非 string 时返回 `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.contains("access-gateway", "gateway"); // true
```

### `strings.contains_any(text, chars)`

- 参数：`text` 必须是 string；`chars` 是 Unicode code point 集合，而不是 substring。
- 返回：`text` 含 `chars` 中任一字符时为 boolean；空 chars 返回 `false`；类型错误返回 `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.contains_any("route-42", "xyz2"); // true，因为命中字符 "2"
```

### `strings.index(text, value)`

- 参数：两个必填 string；`value` 按完整 substring 查找。
- 返回：首次匹配的 UTF-16 index；找不到返回 integer `-1`；类型错误返回 `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.index("😀-route", "route"); // 3：😀 占两个 UTF-16 code unit
```

### `strings.indexAny(text, chars)`

- 参数：两个必填 string；`chars` 中每个 Unicode code point 都是候选字符。
- 返回：任何候选字符首次出现的 UTF-16 index；找不到返回 `-1`；类型错误返回 `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.indexAny("abc-123", "93c"); // 2，先命中 text 中的 "c"
```

### `strings.lastIndex(text, value)`

- 参数：两个必填 string；`value` 按完整 substring 查找。
- 返回：最后一次匹配的 UTF-16 index；找不到返回 `-1`；空 value 返回 `length(text)`；类型错误返回
  `null`。
- 异常：不抛可捕获异常。

```javascript
return strings.lastIndex("a/b/a", "a"); // 4
```

### `strings.lastIndexAny(text, chars)`

- 参数：两个必填 string；`chars` 按 Unicode code point 顺序遍历。
- 返回：找到的**第一个候选字符**在 `text` 中的最后 UTF-16 index；只有该候选完全不存在时才检查
  `chars` 的下一个字符。所有候选都不存在时返回 `-1`，类型错误返回 `null`。
- 异常：不抛可捕获异常。
- 兼容提醒：结果不是所有候选字符最后位置的最大值。

```javascript
return strings.lastIndexAny("abca", "bc");
// 1：先检查 "b"，其最后位置为 1；不会再选择位置 2 的 "c"
```

### `strings.repeat(text, count)`

- 参数 `text`：必填 string；非 string 返回 `null`。
- 参数 `count`：必须是 integer 或 float。float 向 0 截断并按兼容的 32-bit integer 语义处理；非数值
  或负数返回 `null`。
- 返回：重复 `count` 次的新 string；0 或空 text 返回空 string；1 返回原 string value。
- 异常：不抛可捕获异常。
- 执行中止：结果超过 16 MiB 时以 `OutOfMemory` 中止，不能由 `catch` 捕获。

```javascript
return strings.repeat("ab", 3.9); // "ababab"
```

### `strings.substring(text, start = 0, end = 2147483647)`

- 参数 `text`：必填 string；非 string 返回 `null`。
- 参数 `start`/`end`：UTF-16 index。number 向 0 截断；boolean 转成 0/1；以合法 integer 开头的
  string 读取该 integer 前缀（例如 `"4x"` 按 4）；`null`、`undefined` 和其他值按 0。`start < 0`
  归零。
- 返回：半开区间 `[start, end)` 的 string。`start >= length(text)` 或 `end <= start` 返回空 string；
  `end` 超过长度时截到末尾。
- 异常：不抛可捕获异常。

```javascript
return {
    normal: strings.substring("gateway", 0, 4), // "gate"
    clamped: strings.substring("gateway", -5, 4), // "gate"
    converted: strings.substring("gateway", "4", 99) // "way"
};
```

### `strings.toString()` / `strings.toString(value)`

- 参数：允许 0 或 1 个。无参数返回空 string。
- 返回：string。`null`/`undefined` 为 `"null"`；string、number、boolean 使用文本值；Binary 使用原始
  byte 的兼容文本；array/object 分别为字面量 `"<ArrayNode>"`/`"<ObjectNode>"`。
- 异常：不抛可捕获异常。
- 提醒：这不是 JSON encoder；object、array 或需要稳定 wire format 的数据必须使用
  `JSON.stringify()`。

```javascript
return {
    empty: strings.toString(), // ""
    missing: strings.toString(null), // "null"
    object: strings.toString({ ok: true }), // "<ObjectNode>"
    json: JSON.stringify({ ok: true }) // "{\"ok\":true}"
};
```

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

### `binary.base64Encode(value)`

- 参数 `value`：必填；只有 Binary 会被编码。
- 返回：标准 Base64 string，包含需要的 `=` padding；0-byte Binary 返回空 string。非 Binary 返回
  `undefined`。
- 异常：不抛可捕获异常；类型错误不是异常。

```javascript
let bytes = binary.fromHex("48656c6c6f");
return binary.base64Encode(bytes); // "SGVsbG8="
```

### `binary.base64Decode(value)`

- 参数 `value`：必填；只有 string 会被解码。
- 返回：Binary；空 string 返回 0-byte Binary。非 string 返回 `undefined`。
- 异常：非法 Base64 字符、错误或多余 padding、非 4 倍数长度、夹杂 whitespace 时抛
  `RangeError`。

```javascript
try {
    return binary.hex(binary.base64Decode("AP8=")); // "00ff"
} catch (e) {
    return "invalid base64";
}
```

### `binary.hex(value)`

- 参数 `value`：必填 Binary。
- 返回：每个 byte 两个字符的小写 hexadecimal string；0-byte Binary 返回空 string。
- 异常：非 Binary 抛 `TypeError`。

```javascript
return binary.hex(binary.base64Decode("AQID")); // "010203"
```

### `binary.fromHex(value)`

- 参数 `value`：必填 string；大小写 hex digit 都接受。
- 返回：Binary；每两个 hex digit 生成一个 byte，空 string 返回 0-byte Binary。
- 异常：非 string 抛 `TypeError`；长度为奇数或出现非 hex digit 时抛 `RangeError`。

```javascript
return binary.base64Encode(binary.fromHex("48656C6C6F")); // "SGVsbG8="
```

### `binary.getUtf8Bytes(value)`

- 参数 `value`：必填任意脚本值。
- 返回：兼容文本表示的 byte：string 使用 UTF-8 byte；number、boolean、`null` 使用各自文本；
  `undefined` 使用空文本；Binary 保留原始 byte；array/object 使用
  `"<ArrayNode>"`/`"<ObjectNode>"`。
- 异常：不抛可捕获异常。
- 提醒：它不是 JSON-to-bytes。JSON wire byte 应使用
  `binary.getUtf8Bytes(JSON.stringify(value))`。当前没有通用 Binary-to-UTF-8-string decoder；解析请求
  JSON 应直接使用 `req.readJson()`。

```javascript
return {
    text: binary.hex(binary.getUtf8Bytes("Hi")), // "4869"
    json: binary.hex(binary.getUtf8Bytes(JSON.stringify({ ok: true })))
};
```

## 9. Hash 函数

```typescript
namespace hash {
    function crc32(value: ScriptValue): number;
    function md5(value: string | Binary): string;
    function sha1(value: string | Binary): string;
    function sha256(value: string | Binary): string;
}
```

### `hash.crc32(value)`

- 参数 `value`：必填任意值。string 按 UTF-8 byte；integer、float、boolean、`null` 按文本；
  `undefined`、array、object 和 Binary 按空文本。
- 返回：integer，范围 `0..4294967295`。空文本返回 0。
- 异常：不抛可捕获异常。

```javascript
return hash.crc32("123456789"); // 3421780262（0xcbf43926）
```

### `hash.md5(value)`

- 参数 `value`：必填 UTF-8 string 或 Binary；Binary 直接使用原始 byte。
- 返回：32 个字符的小写 hexadecimal string。
- 异常：其他参数类型抛 `TypeError`；底层摘要算法出现不可恢复故障时会以 `HostFault` 中止。
- 安全：仅用于旧协议互操作，不能用于密码、签名或抗碰撞用途。

```javascript
return hash.md5("abc"); // "900150983cd24fb0d6963f7d28e17f72"
```

### `hash.sha1(value)`

- 参数 `value`：必填 UTF-8 string 或 Binary。
- 返回：40 个字符的小写 hexadecimal string。
- 异常：其他参数类型抛 `TypeError`；底层摘要算法出现不可恢复故障时会以 `HostFault` 中止。
- 安全：仅用于旧协议互操作，不用于新的安全设计。

```javascript
return hash.sha1("abc"); // "a9993e364706816aba3e25717850c26c9cd0d89d"
```

### `hash.sha256(value)`

- 参数 `value`：必填 UTF-8 string 或 Binary。
- 返回：64 个字符的小写 hexadecimal string。
- 异常：其他参数类型抛 `TypeError`；底层摘要算法出现不可恢复故障时会以 `HostFault` 中止。
- 安全：这是无密钥摘要，不是 MAC。需要认证时不能用 `sha256(secret + data)` 替代经过审核的 HMAC。

```javascript
return hash.sha256("abc");
// "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
```

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

### `math.floor(value)`

- 参数 `value`：必填 integer 或 float。
- 返回：integer 输入原样返回；有限 float 向负无穷取整并返回 integer。
- 异常：非 number 抛 `TypeError`。

```javascript
return { positive: math.floor(3.9), negative: math.floor(-1.2) };
// {"positive":3,"negative":-2}
```

### `math.abs(value)`

- 参数 `value`：必填 integer 或 float。
- 返回：与输入数值类别一致的绝对值。为 Java 兼容，最小 64-bit integer
  `-9223372036854775808` 无可表示的正值，因此原样返回。
- 异常：非 number 抛 `TypeError`。

```javascript
return { integer: math.abs(-7), floating: math.abs(-1.25) };
// {"integer":7,"floating":1.25}
```

### `rand.random(max = 1000)`

- 参数 `max`：可选 integer 或 float，默认 1000。float 向 0 截断；极大 float 按 64-bit 边界饱和。
- 返回：在半开区间 `[0, max)` 内均匀采样的 integer。
- 异常：非 number 抛 `TypeError`；转换后的 `max <= 0` 抛 `RangeError`。
- 稳定性：每个 worker thread 独立、非确定性；不是密码学安全随机源。

```javascript
let shard = rand.random(16); // 0 到 15
return shard;
```

### `rand.canary(ratio, ...keys)`

- 参数 `ratio`：必填，目标百分比。integer/float 按 integer 使用，boolean 转成 1/0，其他类型按 0。
- 参数 `keys`：零个或多个任意值。有 key 时按参数顺序把非空兼容文本累计输入 CRC-32；`null` 文本为
  `"null"`，`undefined`/container/Binary 的文本为空并被跳过。key 之间不插入分隔符。
- 返回：`ratio <= 0` 为 `false`，`ratio >= 100` 为 `true`。中间比例无 key 时随机分桶；有 key 时稳定
  映射到 `0..99`，同一参数序列在相同实现上得到相同结果。
- 异常：不抛可捕获异常。
- 使用边界：适合脚本内非安全抽样，但不能替代 Access Gateway 的 production gray/service selection，
  也不能用于安全授权、令牌或密码学随机。

```javascript
let selected = rand.canary(5, $header.x_user_id, $path.id);
if (selected) {
    resp.setHeader("X-Canary", "1");
}
return selected;
```

## 11. JSON 函数

```typescript
namespace JSON {
    function parse(text: string): ScriptValue;
    function stringify(value: ScriptValue): string | undefined;
}
```

### `JSON.parse(text)`

- 参数 `text`：必填 string；不会先调用 `strings.toString()` 做隐式转换。
- 返回：JSON document 对应的 ScriptValue，可以是 object、array 或 scalar。
- 异常：非 string 抛 `TypeError`；空 string、尾随非法内容、格式错误或超过 decoder 限制时抛
  `SyntaxError`。解析异常携带 decoder message 和失败 byte offset。
- 副作用：无。

```javascript
try {
    let value = JSON.parse('{"enabled":true,"weight":10}');
    return value.weight; // 10
} catch (e) {
    return null;
}
```

### `JSON.stringify(value)`

- 参数 `value`：必填任意脚本值。
- 返回：JSON text string。顶层 `undefined` 返回脚本值 `undefined`，不是 string；object/array 内的
  `undefined` 编码为 JSON `null`。顶层 `NaN` 和正负 infinity 返回 string `"null"`。Binary 编码为
  Base64 JSON string；object property 保持插入顺序。
- 异常：无法编码的内部值、非法 string、超过最大嵌套深度，或嵌套的不可编码 number/value 抛
  `TypeError`。
- 提醒：该函数只产生 string，不设置 HTTP header。直接发送 JSON 应使用 `resp.sendJson()`。

```javascript
let encoded = JSON.stringify({
    ok: true,
    missing: undefined,
    bytes: binary.fromHex("0102")
});
return encoded;
// "{\"ok\":true,\"missing\":null,\"bytes\":\"AQI=\"}"
```

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

### `Object.assign(target, source, ...sources)`

- 参数 `target`：必填 object，作为写入目标。
- 参数 `source`/`sources`：至少提供一个 source；只有 object source 的自有属性会复制，非 object source
  静默跳过。
- 返回：被修改的 `target` 本身。相同 key 后值覆盖前值；覆盖已有 key 不改变其插入顺序，新 key
  追加到末尾。复制的是 ScriptValue 引用语义，不做递归 deep merge。
- 异常：`target` 非 object 时抛 `TypeError`。
- 副作用：原地修改 `target`。

```javascript
let target = { a: 1, keep: true };
let same = Object.assign(target, { a: 2 }, null, { b: 3 });
return { value: target, sameObject: same === target };
// {"value":{"a":2,"keep":true,"b":3},"sameObject":true}
```

### `Object.keys(value)`

- 参数 `value`：必填 object。
- 返回：包含所有自有 property name 的新 string array，顺序为属性插入顺序。
- 异常：非 object 抛 `TypeError`。
- 副作用：不修改 object。

```javascript
return Object.keys({ first: 1, second: 2 }); // ["first", "second"]
```

### `Object.values(value)`

- 参数 `value`：必填 object。
- 返回：包含所有自有 property value 的新 array，顺序与 `Object.keys()` 一致。
- 异常：非 object 抛 `TypeError`。
- 副作用：不修改 object；返回 array 中的 container value 不是 deep copy。

```javascript
return Object.values({ first: 1, second: "two" }); // [1, "two"]
```

### `Object.deleteProperties(target, key, ...keys)`

- 参数 `target`：必填 object。
- 参数 `key`/`keys`：至少一个待删除值；只有 string 被当作 property name，非 string 和不存在的 key
  静默跳过。
- 返回：被修改的 `target` 本身。
- 异常：`target` 非 object 时抛 `TypeError`。
- 副作用：按参数顺序原地删除属性。

```javascript
let value = { password: "redact", visible: true, token: "redact" };
Object.deleteProperties(value, "password", 123, "token", "missing");
return value; // {"visible":true}
```

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

### `URL.encodeComponent(value)`

- 参数 `value`：必填 string；按 UTF-8 byte 编码。
- 返回：form-urlencoded component string。`A-Z a-z 0-9 - _ . *` 原样保留，ASCII space 变成
  `+`，其余 byte 变成大写 `%HH`。
- 异常：非 string 抛 `TypeError`。
- 提醒：与 ECMAScript `encodeURIComponent` 不同，后者把 space 编码为 `%20`。

```javascript
return URL.encodeComponent("a b/中"); // "a+b%2F%E4%B8%AD"
```

### `URL.decodeComponent(value)`

- 参数 `value`：必填 string。
- 返回：解码后的 string；`+` 变成 space，合法 `%HH` 变成对应 byte。解码后的非法 UTF-8 会以
  U+FFFD replacement character 修复。
- 异常：非 string 抛 `TypeError`；不完整或非 hex 的 percent escape 抛 `RangeError`。

```javascript
return URL.decodeComponent("a+b%2F%E4%B8%AD"); // "a b/中"
```

### `URL.parseQuery(value)`

- 参数 `value`：必填、不带前导 `?` 的 query string。
- 返回：新 object，所有 value 都是 string 或 string array。`&` 分段，第一个 `=` 分隔 key/value；
  无 `=` 的非空段得到空 string value；空段跳过。重复 key 首次是 string，第二次提升为 array，后续按
  顺序追加。
- 异常：非 string 抛 `TypeError`；任意 key/value 含非法 percent escape 时抛 `RangeError`，不返回
  partial object。

```javascript
return URL.parseQuery("tag=a&tag=b&page=2&flag");
// {"tag":["a","b"],"page":"2","flag":""}
```

### `URL.buildQuery(value = undefined)`

- 参数 `value`：可选。object property name 成为 key；scalar value 用兼容文本；array value 展开为
  同名重复字段；空 array 不产生字段。
- 返回：按 property 插入顺序生成、不带前导 `?` 的 form-urlencoded string，每个字段都含 `=`。
  空 object 返回空 string。顶层 `null`/`undefined` 原样返回，因此返回值不一定是 string。
- 异常：除 `null`/`undefined` 外的非 object 抛 `TypeError`。
- 转换：array element 为 object/array 时使用 `"<ObjectNode>"`/`"<ArrayNode>"`；需要 JSON value
  时应先显式 `JSON.stringify()`。

```javascript
return URL.buildQuery({ tag: ["a", "b"], page: 2, empty: null });
// "tag=a&tag=b&page=2&empty=null"
```

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

### `req.getHeader()` / `req.getHeader(name)`

- 无参数返回：本次请求的 header object，并在脚本上下文内缓存。同名字段折叠为一个 string value；如需
  稳定读取某一字段，优先使用单参数 overload。
- 参数 `name`：单参数 overload 要求非空 string，header name 按 HTTP 大小写不敏感规则查找。
- 单参数返回：存在时为 string，不存在时为 `undefined`；name 为空或非 string 时为 `null`。
- 异常：正常输入不抛可捕获异常。

```javascript
let all = req.getHeader();
return {
    contentType: req.getHeader("Content-Type"),
    hostFromObject: all.host
};
```

### `req.getQuery()` / `req.getQuery(name)`

- 无参数返回：解析当前 raw query 得到并缓存的 object，value 为 string；重复 key 保留最后一个值。
  解析使用 form-urlencoded 规则。错误 percent escape 不抛异常，而是保留错误发生前已解析的字段。
- 参数 `name`：单参数 overload 要求非空 string，key 匹配大小写敏感。
- 单参数返回：存在时为 string，不存在、空 name 或非 string name 时为 `undefined`。
- 异常：不抛可捕获异常。

```javascript
// 对 /search?q=fiber&page=2
return {
    query: req.getQuery("q"), // "fiber"
    page: req.getQuery().page, // "2"
    missing: req.getQuery("missing") // undefined
};
```

### `req.getCookie()` / `req.getCookie(name)`

- 无参数返回：解析所有 `Cookie` header 后缓存的 object；cookie value 为 string，同名 cookie 后值覆盖
  前值。parser 接受用 `;` 分隔的宽松 cookie pair。
- 参数 `name`：单参数 overload 要求非空 string；cookie name 匹配大小写敏感。
- 单参数返回：存在时为 string；不存在、空 name 或非 string name 时为 `undefined`。
- 异常：不抛可捕获异常。

```javascript
let cookies = req.getCookie();
return {
    session: req.getCookie("session"),
    theme: cookies.theme
};
```

### `req.getUri()`

- 参数：无。
- 返回：raw request target string，格式为 `path[?query]`；不含 scheme、authority/Host 或 fragment。
- 异常：不抛可捕获异常。

```javascript
// 对 /orders/42?expand=items
return req.getUri(); // "/orders/42?expand=items"
```

### `req.getPath()`

- 参数：无。
- 返回：HTTP parser 得到的 path string，不含 `?` 和 query。
- 异常：不抛可捕获异常。

```javascript
return req.getPath(); // 例如 "/orders/42"
```

### `req.getQueryStr()`

- 参数：无。
- 返回：不含前导 `?` 的 raw query string；请求没有 query 时返回空 string。它不会 form-decode。
- 异常：不抛可捕获异常。

```javascript
return req.getQueryStr(); // 例如 "expand=items&lang=zh-CN"
```

### `req.getMethod()`

- 参数：无。
- 返回：当前请求 method token string，例如 `"GET"`、`"POST"`。
- 异常：不抛可捕获异常。

```javascript
if (req.getMethod() !== "POST") {
    resp.sendJson(405, { error: "METHOD_NOT_ALLOWED" });
    return;
}
```

### `req.readJson()`

- 参数：无；异步宿主函数，但脚本中直接调用，不使用 `await`。
- 返回：完整消费 request body 后解析出的任意 JSON ScriptValue。
- 异常：body 读取失败抛 `Error("read request body failed")`；空 body 抛
  `Error("client did not sent body")`；非法 JSON 抛 `Error("invalid json body")`。它不会暴露
  `JSON.parse()` 那样的 `SyntaxError` 细节。
- 副作用：一次性消费 request body。不要再调用任何 body reader 或依赖原 body 的操作。

```javascript
try {
    let body = req.readJson();
    return { name: body.name, accepted: true };
} catch (e) {
    resp.sendJson(400, { error: "INVALID_JSON_BODY" });
    return;
}
```

### `req.readBinary()`

- 参数：无；异步宿主函数。
- 返回：完整 request body 的 Binary；空 body 返回 0-byte Binary。
- 异常：body 读取失败抛 `Error("read request body failed")`。
- 副作用：一次性消费 request body，并把全部 byte 连续化到内存。

```javascript
let body = req.readBinary();
return {
    size: length(body),
    sha256: hash.sha256(body)
};
```

### `req.discardBody()`

- 参数：无；异步宿主函数。
- 返回：`null`。
- 异常：当前兼容实现忽略底层 drain error，不抛可捕获异常。
- 副作用：消费并丢弃剩余 request body；常用于提前返回前保持连接复用条件。

```javascript
req.discardBody();
resp.send(204);
return;
```

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

### `resp.setHeader(name, value)`

- 参数 `name`：必填、非空 string。
- 参数 `value`：必填。string、integer、float、boolean 和 `null` 按兼容文本写入；空 string、
  `undefined`、array、object 或 Binary 会得到空文本并视为无效。
- 返回：`null`。
- 异常：name 非 string/空，或转换后的 value 为空时抛
  `Error("set header require string key value")`。
- 副作用：替换 pending response header 的同名值。header 已提交后，合法调用静默不再修改响应。

```javascript
resp.setHeader("Cache-Control", "no-store");
resp.setHeader("X-Route-Version", 7);
return { ok: true };
```

### `resp.addHeader(name, value)`

- 参数及文本转换与 `resp.setHeader()` 相同。
- 返回：`null`。
- 异常：name/value 无效时抛 `Error("add header require string key value")`。
- 副作用：追加 header field，不替换已有同名字段；适合多个 `Set-Cookie` 或其他允许重复的 header。
  header 已提交后，合法调用静默不再修改响应。

```javascript
resp.addHeader("Vary", "Accept-Encoding");
resp.addHeader("Vary", "Origin");
return { ok: true };
```

### `resp.addCookie(cookie)`

- 参数 `cookie`：必填 object。字段如下；未知字段忽略。

| 字段       | 接受类型                             | 默认/行为                                     |
| ---------- | ------------------------------------ | --------------------------------------------- |
| `name`     | 非空 string，且必须是 RFC token char | 必填；非法时整个函数返回 `false`              |
| `value`    | 任意 scalar                          | 兼容文本；缺失/不可文本化时为空 string        |
| `domain`   | string                               | 非 string 或空值时省略 `Domain`               |
| `path`     | string                               | 非 string 或空值时省略 `Path`                 |
| `maxAge`   | integer                              | 非 integer 或负数时省略；`0` 生成 `Max-Age=0` |
| `secure`   | boolean                              | 仅严格 boolean `true` 生成 `Secure`           |
| `httpOnly` | boolean                              | 仅严格 boolean `true` 生成 `HttpOnly`         |
| `sameSite` | `"Lax"`、`"Strict"` 或 `"None"`      | 大小写敏感；其他值省略 `SameSite`             |

- 返回：成功编码并加入 pending `Set-Cookie` 时为 `true`；参数非 object、name 缺失/为空/含非法 token
  字符时为 `false`。
- 异常：无函数特有的可捕获异常；无效 cookie 用 `false` 表达。
- 副作用：追加一个 `Set-Cookie`。header 已提交后无法再改变 wire response，因此必须在 `send*()` 前调用。

```javascript
let added = resp.addCookie({
    name: "session",
    value: "abc123",
    path: "/",
    maxAge: 3600,
    secure: true,
    httpOnly: true,
    sameSite: "Lax"
});
return { cookieAdded: added };
```

### `resp.sendJson(status, body)`

- 参数 `status`：必填；只有 integer 被采用，其他类型回退为 200。该函数不预先限制 status 范围，
  无效 HTTP status 最终表现为发送失败。
- 参数 `body`：任意 ScriptValue，使用 HTTP JSON encoder；顶层 `undefined` 编码为 JSON `null`，Binary
  编码为 Base64 JSON string。
- 返回：成功发送 header 和完整 body 后返回 `null`。
- 异常：JSON 无法编码或 header/body 写入失败时抛 `Error("error send json")`。
- 副作用：把 `Content-Type` 设置为 `application/json`，发送固定 `Content-Length` 响应并结束 stream。

```javascript
resp.sendJson(201, {
    id: $path.id,
    created: true
});
return;
```

### `resp.send(status)` / `resp.send(status, body)`

- 参数 `status`：必填；只有 integer 被采用，其他类型回退为 200。
- 参数 `body`：可省略。省略时发送 0-byte body；Binary 原样发送且不自动设置 Content-Type；string
  按 UTF-8 byte 发送并设置 `text/plain;charset=utf-8`；其他值按 JSON 发送并设置
  `application/json`。
- 返回：成功发送后为 `null`。
- 异常：空响应发送失败抛 `Error("error send")`；Binary 写入失败抛
  `Error("error write binary response")`；text 写入失败抛 `Error("error textual response")`；JSON
  编码/写入失败抛 `Error("error send json")`。
- 副作用：提交 response header 并结束 stream。一次正常执行只调用一个 `send*()`，调用后立即
  `return;`。

```javascript
if (req.getMethod() === "HEAD") {
    resp.send(204);
    return;
}

resp.setHeader("Content-Type", "application/octet-stream");
resp.send(200, binary.fromHex("89504e47"));
return;
```

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
