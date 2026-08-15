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

### 4.1 How to read the function entries

Signatures use TypeScript-like notation for documentation only; the script language does not implement
TypeScript. `?` or `= default` marks an optional argument, and `...items` marks variadic arguments. Function
names and argument counts resolve at **configuration compile time**:

- a missing required argument, an extra fixed argument, an unknown function, or an unresolved overload rejects
  the candidate configuration; this is not a runtime exception and `try/catch` cannot catch it;
- the runtime inserts the documented value for omitted optional arguments; a variadic list may be empty and may
  use `...array` expansion;
- argument types are generally checked at **execution time**. Every entry's “Exceptions” line states whether a
  wrong type returns `null`, `undefined`, or `false`, or raises a catchable exception;
- “returns the original object/array” means in-place mutation with the same identity, not a copy;
- unless stated otherwise, string indices and lengths use UTF-16 code units, while Binary lengths use bytes.

### 4.2 Exceptions and execution aborts are different

| Result                      | Catchable | Final JavaScript Route handling                             |
| --------------------------- | --------- | ----------------------------------------------------------- |
| `TypeError`                 | Yes       | Uncaught: 500 `SCRIPT_EXECUTION`                            |
| `RangeError`                | Yes       | Uncaught: 500 `SCRIPT_EXECUTION`                            |
| `SyntaxError`               | Yes       | Uncaught: 500 `SCRIPT_EXECUTION`                            |
| `Error`                     | Yes       | HTTP read/encode/write failures; uncaught: 500              |
| `Abort`                     | No        | `OutOfMemory`, `Timeout`, `Cancelled`, `InvalidState`, etc. |
| Configuration compile error | No        | Candidate snapshot rejected; previous snapshot remains live |

An entry's “Exceptions” line describes catchable script exceptions. Any function that allocates a result string,
Binary, array, or object can abort with `OutOfMemory` when resources are exhausted. Async HTTP functions may also
be aborted by host cancellation or timeout. An abort is not a script `throw` and never enters `catch`. Entries
repeat abort behavior only where the function has an additional stable limit.

```javascript
let decoded;
try {
    decoded = binary.base64Decode("not-base64");
} catch (e) {
    // RangeError is caught here.
    decoded = binary.fromHex("");
}
```

## 5. General functions

```typescript
function length(value?: ScriptValue): number;
function includes(container: string | ScriptValue[], ...items: ScriptValue[]): boolean;
```

### `length(value = null)`

- Parameter: `value` may be any script value and defaults to `null` when omitted.
- Returns: integer. A string returns its UTF-16 code-unit count; Binary returns bytes; an array returns elements;
  an object returns own properties. `null`, `undefined`, numbers, booleans, and internal-only kinds return 0.
- Exceptions: none.
- Side effects: none.

```javascript
return {
    ascii: length("abc"), // 3
    emoji: length("A😀"), // 3: A is one unit and 😀 is two
    bytes: length(binary.fromHex("00ff10")), // 3
    fields: length({ a: 1, b: 2 }), // 2
    omitted: length() // 0
};
```

### `includes(container, ...items)`

- Parameter `container`: required; only a string or array has matching behavior.
- Parameter `items`: zero or more values to test.
- Returns: boolean. For a string, every item must itself be a string and a substring. For an array, every item
  must be present under strict `===` equality. Another container type, or a non-string item against a string,
  returns `false`. A valid string/array with no items returns `true`.
- Exceptions: none.
- Side effects: none.

```javascript
return {
    text: includes("access-gateway", "access", "way"), // true
    methods: includes(["GET", "POST"], req.getMethod()),
    strict: includes([1], "1"), // false: 1 !== "1"
    emptyCheck: includes([]) // true
};
```

## 6. Array functions

```typescript
namespace array {
    function join(values: ScriptValue[], separator?: ScriptValue): string;
    function pop(values: ScriptValue[]): ScriptValue | null;
    function push(values: ScriptValue[], ...items: ScriptValue[]): ScriptValue[];
}
```

### `array.join(values, separator = "")`

- Parameter `values`: required array.
- Parameter `separator`: optional and defaults to an empty string, not JavaScript's comma. Strings, integers,
  floats, and booleans become text; `null`, `undefined`, arrays, objects, and Binary become empty text.
- Returns: a new string. Each element uses the same compatibility-text conversion as the separator.
- Exceptions: throws `TypeError` when `values` is not an array.
- Side effects: does not modify the input array.

```javascript
return array.join(["route", 7, null, true], "|");
// "route|7||true"
```

### `array.pop(values)`

- Parameter `values`: required array.
- Returns: the former last element; an empty array returns `null`.
- Exceptions: throws `TypeError` when `values` is not an array.
- Side effects: removes one element in place; an empty array is unchanged.

```javascript
let values = ["a", "b"];
let last = array.pop(values);
return { last: last, remaining: values };
// {"last":"b","remaining":["a"]}
```

### `array.push(values, ...items)`

- Parameter `values`: required array.
- Parameter `items`: zero or more values; `...anotherArray` expansion is supported.
- Returns: the modified `values` array itself, **not** its new length.
- Exceptions: throws `TypeError` when `values` is not an array.
- Side effects: appends items in argument order; no items leaves the array unchanged.

```javascript
let values = [1];
let same = array.push(values, 2, ...[3, 4]);
return { values: values, sameObject: same === values };
// {"values":[1,2,3,4],"sameObject":true}
```

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

### `strings.hasPrefix(text, prefix)`

- Parameters: both `text` and `prefix` are required strings.
- Returns: boolean `true` when `text` starts with `prefix`; otherwise `false`. An empty prefix matches every
  valid string.
- Exceptions: none; a non-string argument returns `false`.

```javascript
return strings.hasPrefix("/api/v1/orders", "/api/"); // true
```

### `strings.hasSuffix(text, suffix)`

- Parameters: both `text` and `suffix` are required strings.
- Returns: boolean `true` when `text` ends with `suffix`; otherwise `false`. An empty suffix matches every valid
  string.
- Exceptions: none; a non-string argument returns `false`.

```javascript
return strings.hasSuffix("archive.tar.gz", ".gz"); // true
```

### `strings.toLower(text)`

- Parameter `text`: required string.
- Returns: a new string with ASCII `A-Z` changed to `a-z`. Multibyte UTF-8 characters are unchanged. A
  non-string returns `null`.
- Exceptions: none.

```javascript
return strings.toLower("API-ÄBC-中文"); // "api-Äbc-中文"
```

### `strings.toUpper(text)`

- Parameter `text`: required string.
- Returns: a new string with ASCII `a-z` changed to `A-Z`. Non-ASCII characters remain unchanged. A non-string
  returns `null`.
- Exceptions: none.

```javascript
return strings.toUpper("api-äbc-中文"); // "API-äBC-中文"
```

### `strings.trim(text, cutset = null)`

- Parameter `text`: required string; a non-string returns `null`.
- Parameter `cutset`: optional. Omitted, `null`, or another non-string removes bytes `<= 0x20` from both ends.
  A string is treated as one **complete substring** repeatedly removed from both ends.
- Returns: the trimmed string. An empty cutset removes nothing.
- Exceptions: none.

```javascript
return {
    whitespace: strings.trim(" \t value \n"), // "value"
    marker: strings.trim("ababvalueabab", "ab") // "value"
};
```

### `strings.trimLeft(text, cutset = null)`

- Parameter `text`: required string; a non-string returns `null`.
- Parameter `cutset`: omitted/non-string removes ASCII Java whitespace in
  `0x09..0x0d` and `0x1c..0x20`; a string is repeatedly removed as one complete substring from the left.
- Returns: a new string without changing the original value.
- Exceptions: none.

```javascript
return strings.trimLeft("///v1/items///", "/"); // "v1/items///"
```

### `strings.trimRight(text, cutset = null)`

- Parameters follow `trimLeft()`, but only the right edge is processed.
- Returns: the processed string. An empty cutset removes nothing; a non-string `text` returns `null`.
- Exceptions: none.

```javascript
return strings.trimRight("///v1/items///", "/"); // "///v1/items"
```

### `strings.split(text, separators = null)`

- Parameter `text`: required string; a non-string returns `null`.
- Parameter `separators`: omitted/non-string performs no split and returns an array containing the original
  string. A string is a set of Unicode code points, each acting as an independent separator rather than one
  delimiter substring.
- Returns: a new string array. Consecutive, leading, and trailing separators produce no empty entries. An
  explicit empty separator matches nothing, so non-empty text returns `[text]`, while empty text returns `[]`.
- Exceptions: none.

```javascript
return {
    parts: strings.split("a,b;;c,", ",;"), // ["a", "b", "c"]
    untouched: strings.split("a,b") // ["a,b"]
};
```

### `strings.contains(text, value)`

- Parameters: required strings; `value` is matched as a complete substring.
- Returns: boolean `true` when found and `false` otherwise. An empty value returns `true`. A non-string argument
  returns `null`.
- Exceptions: none.

```javascript
return strings.contains("access-gateway", "gateway"); // true
```

### `strings.contains_any(text, chars)`

- Parameters: `text` is a required string; `chars` is a set of Unicode code points, not a substring.
- Returns: whether any candidate character occurs. Empty `chars` returns `false`; a wrong type returns `null`.
- Exceptions: none.

```javascript
return strings.contains_any("route-42", "xyz2"); // true because "2" matches
```

### `strings.index(text, value)`

- Parameters: two required strings; `value` is matched as a complete substring.
- Returns: the first match's UTF-16 index, integer `-1` when absent, or `null` for a wrong type.
- Exceptions: none.

```javascript
return strings.index("😀-route", "route"); // 3 because 😀 occupies two UTF-16 units
```

### `strings.indexAny(text, chars)`

- Parameters: two required strings; each Unicode code point in `chars` is a candidate.
- Returns: the UTF-16 index of the first occurrence of any candidate, `-1` when absent, or `null` for a wrong
  type.
- Exceptions: none.

```javascript
return strings.indexAny("abc-123", "93c"); // 2: "c" occurs first in text
```

### `strings.lastIndex(text, value)`

- Parameters: two required strings; `value` is matched as a complete substring.
- Returns: the last match's UTF-16 index, `-1` when absent, `length(text)` for an empty value, or `null` for a
  wrong type.
- Exceptions: none.

```javascript
return strings.lastIndex("a/b/a", "a"); // 4
```

### `strings.lastIndexAny(text, chars)`

- Parameters: two required strings. Candidates are visited in Unicode code-point order within `chars`.
- Returns: the last UTF-16 index of the **first candidate character** that occurs in `text`. Only when that
  candidate is absent does the function examine the next candidate. Returns `-1` when all are absent and `null`
  on a wrong type.
- Exceptions: none.
- Compatibility note: this is not the global maximum position across all candidates.

```javascript
return strings.lastIndexAny("abca", "bc");
// 1: "b" is the first candidate and its last position is 1; "c" at 2 is not examined
```

### `strings.repeat(text, count)`

- Parameter `text`: required string; a non-string returns `null`.
- Parameter `count`: integer or float. A float truncates toward zero and follows compatible 32-bit integer
  semantics. A non-number or negative value returns `null`.
- Returns: a new string repeated `count` times. Zero or empty text returns empty; one returns the original string
  value.
- Exceptions: none.
- Execution abort: an output over 16 MiB aborts with `OutOfMemory`; `catch` cannot intercept it.

```javascript
return strings.repeat("ab", 3.9); // "ababab"
```

### `strings.substring(text, start = 0, end = 2147483647)`

- Parameter `text`: required string; a non-string returns `null`.
- Parameters `start`/`end`: UTF-16 indices. Numbers truncate toward zero; booleans become 0/1; a string that
  begins with a valid integer uses that integer prefix (for example, `"4x"` becomes 4); `null`, `undefined`, and
  other values become 0. Negative start clamps to 0.
- Returns: the half-open range `[start, end)`. `start >= length(text)` or `end <= start` returns empty; an end
  beyond the length clamps to the end of the string.
- Exceptions: none.

```javascript
return {
    normal: strings.substring("gateway", 0, 4), // "gate"
    clamped: strings.substring("gateway", -5, 4), // "gate"
    converted: strings.substring("gateway", "4", 99) // "way"
};
```

### `strings.toString()` / `strings.toString(value)`

- Parameters: either zero or one. Zero arguments returns an empty string.
- Returns: string. `null`/`undefined` become `"null"`; strings, numbers, and booleans use text; Binary uses its
  raw-byte compatibility text; arrays/objects become literal `"<ArrayNode>"`/`"<ObjectNode>"`.
- Exceptions: none.
- Note: this is not a JSON encoder. Use `JSON.stringify()` for objects, arrays, or stable wire data.

```javascript
return {
    empty: strings.toString(), // ""
    missing: strings.toString(null), // "null"
    object: strings.toString({ ok: true }), // "<ObjectNode>"
    json: JSON.stringify({ ok: true }) // "{\"ok\":true}"
};
```

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

### `binary.base64Encode(value)`

- Parameter `value`: required; only Binary is encoded.
- Returns: a standard Base64 string with required `=` padding; zero-byte Binary returns empty. A non-Binary
  value returns `undefined`.
- Exceptions: none; a wrong type is not exceptional.

```javascript
let bytes = binary.fromHex("48656c6c6f");
return binary.base64Encode(bytes); // "SGVsbG8="
```

### `binary.base64Decode(value)`

- Parameter `value`: required; only a string is decoded.
- Returns: Binary; an empty string returns zero-byte Binary. A non-string returns `undefined`.
- Exceptions: throws `RangeError` for illegal Base64 characters, incorrect/excess padding, a length not divisible
  by four, or embedded whitespace.

```javascript
try {
    return binary.hex(binary.base64Decode("AP8=")); // "00ff"
} catch (e) {
    return "invalid base64";
}
```

### `binary.hex(value)`

- Parameter `value`: required Binary.
- Returns: a lowercase hexadecimal string with two characters per byte; zero-byte Binary returns empty.
- Exceptions: throws `TypeError` for a non-Binary value.

```javascript
return binary.hex(binary.base64Decode("AQID")); // "010203"
```

### `binary.fromHex(value)`

- Parameter `value`: required string; uppercase and lowercase hex digits are accepted.
- Returns: Binary with one byte per two hex digits; an empty string returns zero-byte Binary.
- Exceptions: throws `TypeError` for a non-string and `RangeError` for odd length or a non-hex digit.

```javascript
return binary.base64Encode(binary.fromHex("48656C6C6F")); // "SGVsbG8="
```

### `binary.getUtf8Bytes(value)`

- Parameter `value`: any required script value.
- Returns: compatibility-text bytes. A string uses UTF-8; numbers, booleans, and `null` use their text;
  `undefined` uses empty text; Binary preserves raw bytes; arrays/objects use
  `"<ArrayNode>"`/`"<ObjectNode>"`.
- Exceptions: none.
- Note: this is not JSON-to-bytes. Use `binary.getUtf8Bytes(JSON.stringify(value))` for JSON wire bytes. There
  is no general Binary-to-UTF-8-string decoder; use `req.readJson()` for a JSON request body.

```javascript
return {
    text: binary.hex(binary.getUtf8Bytes("Hi")), // "4869"
    json: binary.hex(binary.getUtf8Bytes(JSON.stringify({ ok: true })))
};
```

## 9. Hash functions

```typescript
namespace hash {
    function crc32(value: ScriptValue): number;
    function md5(value: string | Binary): string;
    function sha1(value: string | Binary): string;
    function sha256(value: string | Binary): string;
}
```

### `hash.crc32(value)`

- Parameter `value`: any required value. Strings use UTF-8 bytes; integers, floats, booleans, and `null` use
  text; `undefined`, arrays, objects, and Binary use empty text.
- Returns: integer in `0..4294967295`. Empty text returns 0.
- Exceptions: none.

```javascript
return hash.crc32("123456789"); // 3421780262 (0xcbf43926)
```

### `hash.md5(value)`

- Parameter `value`: required UTF-8 string or Binary; Binary uses raw bytes.
- Returns: a 32-character lowercase hexadecimal string.
- Exceptions: throws `TypeError` for another input type. An unrecoverable digest backend failure aborts with
  `HostFault`.
- Security: legacy interoperability only; never use for passwords, signatures, or collision resistance.

```javascript
return hash.md5("abc"); // "900150983cd24fb0d6963f7d28e17f72"
```

### `hash.sha1(value)`

- Parameter `value`: required UTF-8 string or Binary.
- Returns: a 40-character lowercase hexadecimal string.
- Exceptions: throws `TypeError` for another input type. An unrecoverable digest backend failure aborts with
  `HostFault`.
- Security: legacy interoperability only; do not use in new security designs.

```javascript
return hash.sha1("abc"); // "a9993e364706816aba3e25717850c26c9cd0d89d"
```

### `hash.sha256(value)`

- Parameter `value`: required UTF-8 string or Binary.
- Returns: a 64-character lowercase hexadecimal string.
- Exceptions: throws `TypeError` for another input type. An unrecoverable digest backend failure aborts with
  `HostFault`.
- Security: this is an unkeyed digest, not a MAC. Do not substitute `sha256(secret + data)` for a reviewed HMAC.

```javascript
return hash.sha256("abc");
// "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
```

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

### `math.floor(value)`

- Parameter `value`: required integer or float.
- Returns: an integer unchanged, or a finite float rounded toward negative infinity and returned as an integer.
- Exceptions: throws `TypeError` for a non-number.

```javascript
return { positive: math.floor(3.9), negative: math.floor(-1.2) };
// {"positive":3,"negative":-2}
```

### `math.abs(value)`

- Parameter `value`: required integer or float.
- Returns: the absolute value with the same numeric kind. For Java compatibility, the minimum 64-bit integer
  `-9223372036854775808` has no representable positive peer and remains unchanged.
- Exceptions: throws `TypeError` for a non-number.

```javascript
return { integer: math.abs(-7), floating: math.abs(-1.25) };
// {"integer":7,"floating":1.25}
```

### `rand.random(max = 1000)`

- Parameter `max`: optional integer or float, default 1000. A float truncates toward zero; extreme floats
  saturate at 64-bit boundaries.
- Returns: a uniformly sampled integer in the half-open range `[0, max)`.
- Exceptions: throws `TypeError` for a non-number and `RangeError` when converted `max <= 0`.
- Stability: per-worker-thread and nondeterministic; not a cryptographically secure random source.

```javascript
let shard = rand.random(16); // 0 through 15
return shard;
```

### `rand.canary(ratio, ...keys)`

- Parameter `ratio`: required target percentage. Integers/floats become an integer; booleans become 1/0; other
  types become 0.
- Parameter `keys`: zero or more values. With keys, non-empty compatibility text is fed into one cumulative
  CRC-32 in argument order. `null` is text `"null"`; `undefined`, containers, and Binary are empty and skipped.
  No separator is inserted between keys.
- Returns: `false` for `ratio <= 0` and `true` for `ratio >= 100`. An intermediate ratio without keys uses a
  random bucket; with keys it maps deterministically into `0..99` for the same argument sequence and runtime.
- Exceptions: none.
- Scope: suitable for non-security sampling inside a script, but not for authorization, tokens, cryptographic
  randomness, or replacing Access Gateway's production gray/service selection.

```javascript
let selected = rand.canary(5, $header.x_user_id, $path.id);
if (selected) {
    resp.setHeader("X-Canary", "1");
}
return selected;
```

## 11. JSON functions

```typescript
namespace JSON {
    function parse(text: string): ScriptValue;
    function stringify(value: ScriptValue): string | undefined;
}
```

### `JSON.parse(text)`

- Parameter `text`: required string; it is not implicitly converted through `strings.toString()`.
- Returns: the ScriptValue represented by the JSON document, including an object, array, or scalar.
- Exceptions: throws `TypeError` for a non-string. An empty string, malformed/trailing input, or a decoder-limit
  violation throws `SyntaxError` carrying the decoder message and failing byte offset.
- Side effects: none.

```javascript
try {
    let value = JSON.parse('{"enabled":true,"weight":10}');
    return value.weight; // 10
} catch (e) {
    return null;
}
```

### `JSON.stringify(value)`

- Parameter `value`: any required script value.
- Returns: a JSON text string. Top-level `undefined` returns the script value `undefined`, not a string; nested
  `undefined` becomes JSON `null`. Top-level `NaN` and infinities return string `"null"`. Binary becomes a
  Base64 JSON string, and object properties retain insertion order.
- Exceptions: throws `TypeError` for an unencodable internal value, invalid string, maximum-depth violation, or
  an unencodable nested number/value.
- Note: this only creates a string and sets no HTTP headers. Use `resp.sendJson()` for a JSON response.

```javascript
let encoded = JSON.stringify({
    ok: true,
    missing: undefined,
    bytes: binary.fromHex("0102")
});
return encoded;
// "{\"ok\":true,\"missing\":null,\"bytes\":\"AQI=\"}"
```

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

### `Object.assign(target, source, ...sources)`

- Parameter `target`: required object and write target.
- Parameters `source`/`sources`: at least one source argument. Only own properties of object sources are copied;
  a non-object source is silently skipped.
- Returns: the modified `target` itself. A later value overwrites an earlier same-name property; overwriting does
  not change insertion order, while a new property is appended. Values use ScriptValue reference semantics; this
  is not a recursive deep merge.
- Exceptions: throws `TypeError` when `target` is not an object.
- Side effects: modifies `target` in place.

```javascript
let target = { a: 1, keep: true };
let same = Object.assign(target, { a: 2 }, null, { b: 3 });
return { value: target, sameObject: same === target };
// {"value":{"a":2,"keep":true,"b":3},"sameObject":true}
```

### `Object.keys(value)`

- Parameter `value`: required object.
- Returns: a new string array of all own property names in insertion order.
- Exceptions: throws `TypeError` for a non-object.
- Side effects: none.

```javascript
return Object.keys({ first: 1, second: 2 }); // ["first", "second"]
```

### `Object.values(value)`

- Parameter `value`: required object.
- Returns: a new array of all own property values in the same order as `Object.keys()`.
- Exceptions: throws `TypeError` for a non-object.
- Side effects: does not change the object; contained array/object values are not deep-copied.

```javascript
return Object.values({ first: 1, second: "two" }); // [1, "two"]
```

### `Object.deleteProperties(target, key, ...keys)`

- Parameter `target`: required object.
- Parameters `key`/`keys`: at least one value. Only strings are property names; non-strings and absent names are
  silently skipped.
- Returns: the modified `target` itself.
- Exceptions: throws `TypeError` when `target` is not an object.
- Side effects: removes properties in argument order.

```javascript
let value = { password: "redact", visible: true, token: "redact" };
Object.deleteProperties(value, "password", 123, "token", "missing");
return value; // {"visible":true}
```

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

### `URL.encodeComponent(value)`

- Parameter `value`: required string, encoded by UTF-8 byte.
- Returns: a form-urlencoded component. `A-Z a-z 0-9 - _ . *` pass through, ASCII space becomes `+`, and every
  other byte becomes uppercase `%HH`.
- Exceptions: throws `TypeError` for a non-string.
- Note: unlike ECMAScript `encodeURIComponent`, this encodes space as `+`, not `%20`.

```javascript
return URL.encodeComponent("a b/中"); // "a+b%2F%E4%B8%AD"
```

### `URL.decodeComponent(value)`

- Parameter `value`: required string.
- Returns: a decoded string. `+` becomes space and valid `%HH` becomes one byte. Malformed UTF-8 after byte
  decoding is repaired with U+FFFD replacement characters.
- Exceptions: throws `TypeError` for a non-string and `RangeError` for an incomplete or non-hex percent escape.

```javascript
return URL.decodeComponent("a+b%2F%E4%B8%AD"); // "a b/中"
```

### `URL.parseQuery(value)`

- Parameter `value`: required query string without a leading `?`.
- Returns: a new object whose values are strings or string arrays. `&` separates fields and the first `=` splits
  key/value. A non-empty field without `=` gets an empty-string value; empty fields are skipped. A repeated key
  starts as a string, is promoted to an array on its second occurrence, and then appends in order.
- Exceptions: throws `TypeError` for a non-string and `RangeError` if any key/value has a malformed percent
  escape; no partial object is returned.

```javascript
return URL.parseQuery("tag=a&tag=b&page=2&flag");
// {"tag":["a","b"],"page":"2","flag":""}
```

### `URL.buildQuery(value = undefined)`

- Parameter `value`: optional. Object property names become keys; scalar values use compatibility text; array
  values expand into repeated fields; an empty array produces no field.
- Returns: a form-urlencoded string in property insertion order without a leading `?`; every emitted field has
  `=`. An empty object returns empty. Top-level `null`/`undefined` pass through unchanged, so the result is not
  always a string.
- Exceptions: throws `TypeError` for a non-object other than `null`/`undefined`.
- Conversion: object/array elements inside a value array become `"<ObjectNode>"`/`"<ArrayNode>"`. Explicitly
  call `JSON.stringify()` when a query value must carry JSON.

```javascript
return URL.buildQuery({ tag: ["a", "b"], page: 2, empty: null });
// "tag=a&tag=b&page=2&empty=null"
```

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

### `req.getHeader()` / `req.getHeader(name)`

- Zero-argument overload: builds and caches a request-header object for this script context. Repeated fields
  collapse to one string value; prefer the named overload for stable access to one field.
- Parameter `name`: the one-argument overload requires a non-empty string and follows case-insensitive HTTP
  header-name lookup.
- Returns: the cached object without a name; with a name, string when present, `undefined` when absent, and `null`
  for an empty or non-string name.
- Exceptions: none for normal inputs.

```javascript
let all = req.getHeader();
return {
    contentType: req.getHeader("Content-Type"),
    hostFromObject: all.host
};
```

### `req.getQuery()` / `req.getQuery(name)`

- Zero-argument overload: returns a cached object parsed from the raw query with string values; a repeated key keeps its
  last value. Parsing is form-urlencoded. A malformed percent escape does not throw: fields decoded before the
  error remain in the object.
- Parameter `name`: the one-argument overload requires a non-empty string and uses case-sensitive key matching.
- Returns: the cached object without a name; with a name, string when present and `undefined` when absent. An empty
  or non-string name also returns `undefined`.
- Exceptions: none.

```javascript
// For /search?q=fiber&page=2
return {
    query: req.getQuery("q"), // "fiber"
    page: req.getQuery().page, // "2"
    missing: req.getQuery("missing") // undefined
};
```

### `req.getCookie()` / `req.getCookie(name)`

- Zero-argument overload: returns a cached object parsed from all `Cookie` headers. Values are strings and a later
  same-name cookie overwrites an earlier one. The parser accepts lax `;`-separated cookie pairs.
- Parameter `name`: the one-argument overload requires a non-empty string; cookie names are case-sensitive.
- Returns: the cached object without a name; with a name, string when present. Absent, empty, or non-string names
  return `undefined`.
- Exceptions: none.

```javascript
let cookies = req.getCookie();
return {
    session: req.getCookie("session"),
    theme: cookies.theme
};
```

### `req.getUri()`

- Parameters: none.
- Returns: the raw request-target string as `path[?query]`, without scheme, authority/Host, or fragment.
- Exceptions: none.

```javascript
// For /orders/42?expand=items
return req.getUri(); // "/orders/42?expand=items"
```

### `req.getPath()`

- Parameters: none.
- Returns: the parsed path string without `?` or query.
- Exceptions: none.

```javascript
return req.getPath(); // for example, "/orders/42"
```

### `req.getQueryStr()`

- Parameters: none.
- Returns: the raw query without a leading `?`; no query returns empty. No form decoding is performed.
- Exceptions: none.

```javascript
return req.getQueryStr(); // for example, "expand=items&lang=en"
```

### `req.getMethod()`

- Parameters: none.
- Returns: the current request method token, such as `"GET"` or `"POST"`.
- Exceptions: none.

```javascript
if (req.getMethod() !== "POST") {
    resp.sendJson(405, { error: "METHOD_NOT_ALLOWED" });
    return;
}
```

### `req.readJson()`

- Parameters: none. This is an async host function, but script code calls it directly without `await`.
- Returns: any JSON ScriptValue after fully consuming and parsing the request body.
- Exceptions: a read failure raises `Error("read request body failed")`; an empty body raises
  `Error("client did not sent body")`; malformed JSON raises `Error("invalid json body")`. Unlike
  `JSON.parse()`, it does not expose detailed `SyntaxError` parse data.
- Side effects: consumes the one-shot body. Do not call another body reader or operation that requires the
  original body afterward.

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

- Parameters: none; async host function.
- Returns: Binary containing the full request body; an empty body returns zero-byte Binary.
- Exceptions: a body read failure raises `Error("read request body failed")`.
- Side effects: consumes the one-shot body and coalesces all bytes in memory.

```javascript
let body = req.readBinary();
return {
    size: length(body),
    sha256: hash.sha256(body)
};
```

### `req.discardBody()`

- Parameters: none; async host function.
- Returns: `null`.
- Exceptions: the compatibility implementation ignores the underlying drain error and raises no catchable
  exception.
- Side effects: consumes and discards the remaining request body; useful before an early response when preserving
  connection-reuse conditions.

```javascript
req.discardBody();
resp.send(204);
return;
```

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

### `resp.setHeader(name, value)`

- Parameter `name`: required non-empty string.
- Parameter `value`: required. Strings, integers, floats, booleans, and `null` become compatibility text. Empty
  strings, `undefined`, arrays, objects, and Binary produce empty text and are invalid.
- Returns: `null`.
- Exceptions: an invalid name or empty converted value raises
  `Error("set header require string key value")`.
- Side effects: replaces the same-name pending response header. A valid call after headers are committed silently
  makes no wire change.

```javascript
resp.setHeader("Cache-Control", "no-store");
resp.setHeader("X-Route-Version", 7);
return { ok: true };
```

### `resp.addHeader(name, value)`

- Parameters and text conversion match `resp.setHeader()`.
- Returns: `null`.
- Exceptions: an invalid name/value raises `Error("add header require string key value")`.
- Side effects: appends rather than replaces, for fields such as multiple `Set-Cookie` values. A valid call after
  headers are committed silently makes no wire change.

```javascript
resp.addHeader("Vary", "Accept-Encoding");
resp.addHeader("Vary", "Origin");
return { ok: true };
```

### `resp.addCookie(cookie)`

- Parameter `cookie`: required object. Unknown fields are ignored.

| Field      | Accepted type                        | Default/behavior                                      |
| ---------- | ------------------------------------ | ----------------------------------------------------- |
| `name`     | Non-empty RFC token-character string | Required; invalid makes the function return `false`   |
| `value`    | Any scalar                           | Compatibility text; absent/unrenderable becomes empty |
| `domain`   | string                               | Non-string/empty omits `Domain`                       |
| `path`     | string                               | Non-string/empty omits `Path`                         |
| `maxAge`   | integer                              | Non-integer/negative omits it; `0` emits `Max-Age=0`  |
| `secure`   | boolean                              | Only strict boolean `true` emits `Secure`             |
| `httpOnly` | boolean                              | Only strict boolean `true` emits `HttpOnly`           |
| `sameSite` | `"Lax"`, `"Strict"`, or `"None"`     | Case-sensitive; anything else omits `SameSite`        |

- Returns: `true` after successful encoding and insertion into pending `Set-Cookie`; `false` for a non-object or
  missing/empty/invalid-token name.
- Exceptions: no function-specific catchable exception; invalid cookies are represented by `false`.
- Side effects: appends one `Set-Cookie`. Call it before `send*()` because committed headers cannot change.

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

- Parameter `status`: required. Only an integer is used; another type falls back to 200. The function does not
  pre-validate the status range, so an invalid HTTP status ultimately appears as a send failure.
- Parameter `body`: any ScriptValue encoded with the HTTP JSON encoder. Top-level `undefined` becomes JSON
  `null`; Binary becomes a Base64 JSON string.
- Returns: `null` after successfully sending headers and the complete body.
- Exceptions: JSON encoding or header/body write failure raises `Error("error send json")`.
- Side effects: sets `Content-Type: application/json`, sends a fixed-`Content-Length` response, and ends the
  stream.

```javascript
resp.sendJson(201, {
    id: $path.id,
    created: true
});
return;
```

### `resp.send(status)` / `resp.send(status, body)`

- Parameter `status`: required. Only an integer is used; another type falls back to 200.
- Parameter `body`: optional. Omitted sends zero bytes. Binary is raw with no automatic Content-Type; a string is
  UTF-8 with `text/plain;charset=utf-8`; another value is JSON with `application/json`.
- Returns: `null` after a successful send.
- Exceptions: an empty-response failure raises `Error("error send")`; Binary write failure raises
  `Error("error write binary response")`; text failure raises `Error("error textual response")`; JSON
  encoding/write failure raises `Error("error send json")`.
- Side effects: commits response headers and ends the stream. A normal execution calls exactly one `send*()` and
  returns immediately afterward.

```javascript
if (req.getMethod() === "HEAD") {
    resp.send(204);
    return;
}

resp.setHeader("Content-Type", "application/octet-stream");
resp.send(200, binary.fromHex("89504e47"));
return;
```

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
