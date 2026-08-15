# Access Gateway script language, standard library, and HTTP API reference

This is the complete script reference for Access Gateway Route conditions, templates, and JavaScript
Routes. TypeScript below is notation for parameters and return values only. The runtime is not a
TypeScript/JavaScript engine and does not read `.d.ts` files.

The implementation comes from `native/access-server/src/runtime/AccessScriptRuntime.cpp` and the `script`
and `http_script` modules in pinned Fiber revision
`0fda7764bf94944aca4b674ab5ab311184703118`.

## 1. Runtime overview

Fiber Script is a lightweight JS-like bytecode interpreter for gateway configuration:

- tokenization, parsing, bytecode compilation, function resolution, and constant validation happen while
  loading configuration;
- a new immutable Project snapshot is published only after every candidate Route succeeds;
- requests reuse compiled programs, with one heap and `ScriptExchangeCtx` per request;
- synchronous and asynchronous host functions are known at compile time; async calls still have no `await`;
- unknown functions, wrong argument counts, unknown constants, and forbidden capabilities are configuration
  errors.

```javascript
// readJson and sendJson are async host functions, but there is no await syntax.
let body = req.readJson();
resp.sendJson(200, body);
return;
```

Access Gateway provides three contexts:

| Context           | Assignment/control flow        | Async calls                    | `req.*`            | `resp.*` | Request constants |
| ----------------- | ------------------------------ | ------------------------------ | ------------------ | -------- | ----------------- |
| YAML `condition`  | No; one expression             | Rejected                       | Sync metadata only | No       | Yes               |
| Template `${...}` | No; one expression per segment | Rejected                       | Sync metadata only | No       | Yes               |
| JavaScript Route  | Yes; complete script           | Registered async calls allowed | Yes                | Yes      | Yes               |

Outbound HTTP directives are disabled in every Access Gateway context.

## 2. Value types

These approximate types describe script values:

```typescript
declare class Binary {
    private readonly __scriptBinaryBrand: never;
}

type ScriptPrimitive = undefined | null | boolean | number | string | Binary;
type ScriptArray = ScriptValue[];
type ScriptObject = { [key: string]: ScriptValue };
type ScriptValue = ScriptPrimitive | ScriptArray | ScriptObject;
```

### 2.1 Type rules

- numbers internally distinguish signed 64-bit integers and `double`, but both appear as `number`;
- `undefined` means a missing property, uninitialized value, or an API's “no value”; it differs from `null`;
- strings use WTF-8/UTF-16 semantics. `length()`, string indexing, and `strings.substring()` positions are
  UTF-16 code units, so `length("😀") == 2`;
- `Binary` stores raw bytes and usually comes from `req.readBinary()`, `binary.base64Decode()`,
  `binary.fromHex()`, or the host;
- arrays and objects are mutable references. `array.push/pop` and `Object.assign/deleteProperties` mutate;
- object properties preserve insertion order, which controls iteration and JSON output;
- Access Gateway passes `undefined` as a Route script's root `$`. Do not use `$.field`; use `$req`, `$path`,
  and `req.*`.

`typeof` returns runtime-specific names:

```text
undefined, null, boolean, number, string, binary,
array, object, iterator, exception
```

## 3. Lexical and basic syntax

### 3.1 Comments and literals

Both `//` line comments and `/* ... */` block comments are supported, together with these literals:

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

An unterminated string, comment, or template literal is a configuration-time parse error.

### 3.2 Variables, objects, and arrays

Variables use `let`:

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

Object property shorthand, computed properties, and object/array spread are supported. Duplicate static
keys in one object literal are compile errors. Array-index assignment can only replace an existing index;
append with `array.push(list, value)`.

A `let` declaration must end in a semicolon. Terminate every statement to avoid parser ambiguity.

### 3.3 Operators

```text
Unary:       +  -  !  typeof
Arithmetic:  +  -  *  /  %
Comparison:  <  <=  >  >=  ==  !=  ===  !==
Logical:     &&  ||
Membership:  in
Conditional: condition ? whenTrue : whenFalse
Assignment:  =
```

- `&&` and `||` short-circuit and return an operand value, making them useful for defaults;
- if either operand is a string, `+` concatenates strings; objects, arrays, and Binary do not concatenate
  implicitly;
- numeric operations accept numbers, booleans, and `null`, but do not coerce arbitrary strings as ordinary
  JavaScript does;
- `==`/`!=` are loose and `===`/`!==` strict; strict array/object comparison uses reference identity;
- `key in object` tests a property, and `index in array` tests an in-range integer index; other combinations
  are false;
- the parser recognizes the `~` match operator, but the current bytecode compiler rejects it; regex is absent;
- `++`, `--`, compound assignment, optional chaining, and nullish coalescing are unsupported.

```javascript
let role = $header.x_role || "guest";
let allowed = role === "admin" || role === "operator";
return allowed ? { ok: true } : { ok: false };
```

### 3.4 Control flow

Supported statements include `if/else`, two-variable `for-of`, `break`, `continue`, `return`, `try/catch`,
and `throw`:

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

`for-of` has exactly these forms:

```javascript
// Array: key is a zero-based index, value is the element.
for (let key, value of arrayValue) {
    // ...
}

// Object: key/value follow property insertion order.
for (let key, value of objectValue) {
    // ...
}
```

Adding or removing array elements or object properties during iteration is a structural change and raises a
catchable `IterationError`. Updating an existing object property's value is not structural.

There is no `while`, `do/while`, classic three-part `for`, `switch`, function/class declaration, arrow
function, generator, module, or Promise.

### 3.5 Exceptions

```javascript
try {
    let value = JSON.parse(req.getHeader("X-JSON"));
    return { ok: true, value };
} catch (error) {
    return { ok: false };
}
```

`throw value;` can throw any ScriptValue. Type, range, JSON/URL parse, and HTTP semantic errors can also be
caught.

Runtime results distinguish:

- Exception: a semantic script error, catchable by the script and a 500 if uncaught;
- Abort: runtime termination such as OutOfMemory, InvalidState, or Timeout, which cannot be caught.

Do not return the raw caught error to a client because it may expose implementation detail.

### 3.6 Template literals

Script source supports backtick template literals:

```javascript
let id = $path.id;
return `item-${id}-${1 + 2}`;
```

YAML Route templates are a separate configuration layer, without surrounding backticks:

```yaml
rewrite: "/internal/${$path.id}"
response_headers:
    X-Route: "item-${$path.id}"
```

Both compile with configuration. YAML template expressions reject assignment and async functions.

### 3.7 Function calls and arguments

Function names resolve statically at compile time:

```javascript
array.push(items, 1, 2);
array.push(items, ...moreItems);
Object.assign(target, ...sources);
```

An unknown function, wrong fixed argument count, incompatible spread, or overload ambiguity rejects
configuration. There are no dynamic prototype methods: `items.push(x)` is unavailable; use
`array.push(items, x)`.

## 4. Standard-library index

All functions below exist in conditions, templates, and JavaScript Routes; conditions/templates can call
only synchronous functions.

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

## 5. General functions

```typescript
function length(value?: ScriptValue): number;
function includes(container: string | ScriptValue[], ...items: ScriptValue[]): boolean;
```

`length(value)`:

- omitted input is equivalent to `null`;
- strings return UTF-16 code-unit count;
- Binary returns byte count, arrays element count, and objects property count;
- `null`, `undefined`, numbers, booleans, and other values return 0.

`includes(container, ...items)`:

- for a string, every item must be a substring;
- for an array, every item must be found with strict `===` equality;
- other containers return false;
- a valid string/array with no items returns true.

## 6. Array functions

```typescript
namespace array {
    function join(values: ScriptValue[], separator?: ScriptValue): string;
    function pop(values: ScriptValue[]): ScriptValue | null;
    function push(values: ScriptValue[], ...items: ScriptValue[]): ScriptValue[];
}
```

- `array.join()` defaults to an empty separator, not JavaScript's comma. Strings, numbers, and booleans
  become text; `null`, `undefined`, containers, and Binary become empty text. Non-array input throws TypeError.
- `array.pop()` mutates and returns the final element; an empty array returns `null`. Non-array throws TypeError.
- `array.push()` mutates and returns the array itself, not its new length. Non-array throws TypeError.

## 7. String functions

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

For a wrong required string type, most functions return `null`; `hasPrefix()` and `hasSuffix()` return false
instead of throwing.

- `toLower()`/`toUpper()` transform ASCII `A-Z`/`a-z` only; non-ASCII bytes remain unchanged;
- `trim(text)` removes bytes `<= 0x20` from both ends;
- when supplied, `cutset` is a complete substring repeatedly removed from an edge, not a character set;
- without cutset, `trimLeft/trimRight` remove the implementation's ASCII Java whitespace;
- `split(text)` without separators returns `[text]`; with separators it treats them as a Unicode code-point
  set, with no empty entries for consecutive or trailing separators;
- `contains_any/indexAny` treat `chars` as a code-point set;
- `index/indexAny/lastIndex` return UTF-16 indices and -1 when absent;
- compatibility quirk: `lastIndexAny` checks candidate characters in `chars` order and returns the last
  occurrence of the first candidate that exists, not the global maximum across candidates;
- `repeat` truncates a floating count toward zero; negative/non-numeric input returns null. One result is
  limited to 16 MiB, after which execution Aborts;
- `substring` uses UTF-16 indices, defaults to 0 and `2147483647`, clamps negative start to zero, and returns
  empty when `end <= start`;
- zero-argument `toString()` returns empty. `null`/`undefined` return `"null"`; objects/arrays return
  `<ObjectNode>`/`<ArrayNode>`, not JSON. Use `JSON.stringify()` for JSON.

`strings.match` and `strings.findAll` are not registered.

## 8. Binary functions

```typescript
namespace binary {
    function base64Encode(value: Binary): string | undefined;
    function base64Decode(value: string): Binary | undefined;
    function hex(value: Binary): string;
    function fromHex(value: string): Binary;
    function getUtf8Bytes(value: ScriptValue): Binary;
}
```

- `base64Encode` accepts only Binary; other types return undefined;
- `base64Decode` accepts only strings; other types return undefined. Invalid characters, padding, length not
  divisible by four, or whitespace throw RangeError;
- `hex` returns lowercase hex and throws TypeError for non-Binary;
- `fromHex` requires even length and valid hex; invalid content throws RangeError and non-string TypeError;
- `getUtf8Bytes` encodes compatibility text as UTF-8. Objects/arrays become `<ObjectNode>` and `<ArrayNode>`,
  not JSON bytes. For JSON bytes, stringify first;
- there is no general Binary-to-UTF-8-string decoder. Use `req.readJson()` to parse a JSON request body.

## 9. Hash functions

```typescript
namespace hash {
    function crc32(value: ScriptValue): number;
    function md5(value: string | Binary): string;
    function sha1(value: string | Binary): string;
    function sha256(value: string | Binary): string;
}
```

- `crc32` hashes compatibility text and returns `0..0xffffffff`; container and Binary compatibility text is
  empty;
- md5/sha1/sha256 accept UTF-8 strings or raw Binary and return lowercase hex; other types throw TypeError;
- MD5 and SHA-1 are for legacy interoperability only, not signatures, passwords, or collision-resistant use.

## 10. Math and random functions

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

- `math.floor` preserves integers and rounds floating values toward negative infinity; non-number throws
  TypeError;
- `math.abs` returns absolute value; `INT64_MIN` remains unchanged for Java compatibility; non-number throws;
- `rand.random(max)` returns a uniform integer in `[0,max)`, with default 1000 and floating max truncated
  toward zero; `max <= 0` throws RangeError and non-number TypeError;
- `rand.canary(ratio,...keys)` is false at ratio <= 0 and true at ratio >= 100. Without keys it is random;
  with keys it accumulates CRC-32 in argument order for a stable `[0,100)` bucket. Non-numeric ratio is zero;
- `rand.*` is not cryptographic randomness and does not replace production data-plane gray/service selection.

## 11. JSON functions

```typescript
namespace JSON {
    function parse(text: string): ScriptValue;
    function stringify(value: ScriptValue): string | undefined;
}
```

- `JSON.parse` accepts only a string. Invalid JSON throws SyntaxError with a parse message and byte offset;
  non-string throws TypeError;
- `JSON.stringify(undefined)` returns undefined; nested undefined becomes JSON null;
- top-level NaN and infinities encode as text `"null"`; other invalid numbers or unencodable values throw;
- Binary becomes a Base64 JSON string;
- object properties are emitted in insertion order;
- prefer `resp.sendJson()` when producing an HTTP JSON response.

## 12. Object functions

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

- `Object.assign` mutates target with object sources and returns target. Non-object sources are skipped;
  non-object target throws TypeError. Overwrite does not change insertion position.
- `Object.keys/values` return new arrays in insertion order and throw TypeError for non-object.
- `Object.deleteProperties` mutates target by removing string keys and returns target. Non-string or absent keys
  are skipped; non-object target throws TypeError.

## 13. URL form functions

These use `application/x-www-form-urlencoded`, not ECMAScript `encodeURIComponent`: space encodes as `+`,
and `+` decodes as space.

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

- `encodeComponent` preserves alphanumeric, `-`, `_`, `.`, and `*`; space becomes `+`; remaining UTF-8
  bytes use uppercase `%HH`. Non-string throws TypeError.
- `decodeComponent` decodes `+` and `%HH`; bad percent escapes throw RangeError and non-string TypeError.
- `parseQuery` returns an object. A duplicate key promotes a string to an array and keeps appending. A field
  without `=` has empty value; empty segments are skipped.
- `buildQuery` follows object insertion order. Arrays become repeated keys, empty arrays produce no field,
  null/undefined pass through, and other non-object values throw TypeError.

## 14. Request HTTP API: `req.*`

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

- `getHeader()` lazily builds and caches a request-header object; repeated fields collapse to one value;
- `getHeader(name)` uses HTTP name lookup. Missing returns undefined; empty/non-string name returns null;
- `getQuery()` parses the raw query and keeps the last repeated key. On malformed percent escape, values
  parsed before the error remain available;
- `getQuery(name)` returns undefined for absent, empty, or non-string names;
- `getCookie()` parses every Cookie field; a later same-name cookie overwrites an earlier one;
- `getCookie(name)` returns undefined for absent, empty, or non-string names;
- `getUri()` returns raw `path[?query]`, without scheme or host;
- `getPath()` returns parsed path; `getQueryStr()` raw query without `?`; `getMethod()` the original method.

### 14.2 Body

- `readJson()` buffers and parses the full body; empty body, read failure, or invalid JSON throws Error;
- `readBinary()` buffers raw bytes; empty body returns a zero-length Binary;
- `discardBody()` drains and returns null; drain error is ignored for compatibility;
- the body is one-shot, so do not combine or repeat these three calls;
- Access Gateway allows only no body or a known Content-Length within the global limit into SCRIPT. An
  unknown-length body has already failed with 413 before the function can run;
- conditions/templates can call sync `req.*` metadata, but body calls are async and reject compilation.

## 15. Response HTTP API: `resp.*`

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

### 15.1 Headers and cookies

- `setHeader` replaces a pending same-name field; `addHeader` appends;
- empty name or empty compatibility-text value throws Error; after headers are sent, mutation is ignored;
- `addCookie` encodes and appends Set-Cookie. Name is required; maxAge must be integer; secure/httpOnly must
  be boolean; sameSite is case-sensitive. Success returns true and invalid object/field false;
- do not set hop-by-hop/framing headers or output secrets.

### 15.2 Sending

- `sendJson(status,body)` JSON-encodes, sets `application/json`, and sends a fixed-length response. A
  non-integer status falls back to 200; undefined becomes JSON null and Binary a Base64 JSON string;
- `send(status)` sends an empty body;
- `send(status,Binary)` writes raw bytes without an automatic content type;
- `send(status,string)` writes UTF-8 and sets `text/plain;charset=utf-8`;
- `send(status,other)` uses JSON and `application/json`;
- send commits headers and ends the stream. Call one send function and immediately return.

## 16. Route and connection constants

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

Resolution rules:

- `$path.name` must name a current Path capture or configuration compilation fails; an unbound runtime value
  is null;
- `$req` allows only `uri/method/path/query`, and `$conn` only the five fields above. Unknown fields fail;
- `$query.key` and `$context.key` accept any valid script identifier and return null when absent;
- `$header/$cookie/$context` names are ASCII case-insensitive with `-` and `_` normalized;
- a key after `.` must be an identifier; use `req.get*` for special names;
- names are deduplicated into Project compile-time slots and bound by index per request. A known single-value
  constant is usually cheaper than constructing a complete object.

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

## 17. Outbound HTTP capability

Access Gateway JavaScript Routes do **not** provide:

```text
fetch(...)
http.request(...)
http.proxyPass(...)
directive backend = http "@service"
backend.request(...)
backend.proxyPass(...)
```

The pinned Fiber module contains upstream directives that another application host may enable, but
`AccessScriptRuntime` explicitly uses `http_directives_enabled = false` and provides no outbound
`HttpScriptServices`. These names fail configuration compilation rather than bypassing policy at runtime.

Use a YAML `PROXY` Route for upstream traffic so NamingService, static-target validation, protected headers,
body limits, WebSocket behavior, pools, retries, traces, metrics, cancellation, and shutdown all apply.

## 18. Return, errors, and HTTP mapping

Without an explicit send, a JavaScript Route maps results as follows:

| ScriptResult                      | Mapping                |
| --------------------------------- | ---------------------- |
| Value                             | 200 JSON               |
| Void (`return;` or end of source) | 204 empty              |
| Exception / Abort                 | 500 `SCRIPT_EXECUTION` |

After `resp.send*` commits headers, the host does not attempt a second response. More code can still waste
resources or produce invisible errors, so return immediately.

For a condition, Exception/Abort/non-value does not produce 500; it makes that candidate condition false. A
template failure makes its RESPONSE/PROXY preparation fail and enter the stable 500 error path.

A configuration error rejects the complete candidate and retains the previous successful snapshot. A
same-version candidate is ignored. Saving a Draft, publishing to rnacos, and proving instance Active are
distinct; passing Native Validator does not prove any instance activation.

## 19. Quick selection table

| Goal                                     | Preferred API                                          |
| ---------------------------------------- | ------------------------------------------------------ |
| Read one Path capture                    | `$path.id`                                             |
| Read one known query/header/cookie       | `$query.x` / `$header.x` / `$cookie.x`                 |
| Read a key with special characters       | `req.getQuery("a.b")`, etc.                            |
| Enumerate all query/header/cookie values | `req.getQuery()` / `getHeader()` / `getCookie()`       |
| Read JSON                                | `req.readJson()`                                       |
| Read bytes                               | `req.readBinary()`                                     |
| Return JSON                              | `resp.sendJson(status,value)`                          |
| Return text                              | `resp.send(status,text)`                               |
| Return bytes                             | `resp.send(status,binary)` plus explicit Content-Type  |
| Encode/decode a JSON string              | `JSON.stringify/parse`                                 |
| Encode/decode a form query               | `URL.buildQuery/parseQuery`                            |
| Base64                                   | `binary.base64Encode/base64Decode`                     |
| Non-keyed secure digest choice           | `hash.sha256`                                          |
| Stable percentage bucket                 | `rand.canary(ratio,key...)`, for non-security use only |
| Ordinary upstream forwarding             | Do not script; create a YAML `PROXY` Route             |
