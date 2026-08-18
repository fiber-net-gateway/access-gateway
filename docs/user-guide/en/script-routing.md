# Access Gateway script route usage

A script Route reads request data, performs lightweight request-time computation, and directly produces
an HTTP response inside access-server. It is not Node.js, browser JavaScript, or a general edge-functions
platform. Source runs in Fiber's JS-like bytecode interpreter with a fixed function set and explicit body
and lifecycle boundaries.

> Selection rule: use `RESPONSE` for fixed output, `PROXY` for ordinary HTTP/WebSocket forwarding, and a
> JavaScript Route only when request-time branching, JSON assembly, encoding, or hashing is genuinely needed.
> Script Routes cannot currently make outbound HTTP requests.

## 1. Create one in the Console

1. Open the Project **Routes** page.
2. Select **+ JS**.
3. Fill in the external `Path pattern` above the editor.
4. Optionally fill in `Method`; blank means every HTTP method.
5. Choose the optional `Gzip response` setting; `true` uses level 6 and an integer from `1` through `9`
   selects a level.
6. Put only the script source in the JavaScript editor.
7. Fix local issues and save a new immutable configuration version.
8. The server compiles a wire Route and passes it to Native Validator, which uses the same codec and
   compiler as access-server.
9. After creating and executing a Release, observe Published and instance Active separately. Active remains
   unknown without per-instance evidence.

The Console model is approximately:

```typescript
interface JavaScriptRouteItem {
    id: string;
    format: "js";
    path: string;
    method?: string;
    gzip?: boolean | number;
    source: string;
}
```

Publication deterministically compiles it to this rnacos wire shape:

```json
{
    "path": "/diagnostics/:id",
    "method": "POST",
    "type": "SCRIPT",
    "script": "let body = req.readJson(); return {id: $path.id, body};"
}
```

Do not declare `path`, `method`, or `type` in source. Script text is not the authority for match metadata.

## 2. Minimal example

External Path: `/diagnostics/:id`

External Method: `GET`

```javascript
return {
    id: $path.id,
    method: $req.method,
    path: $req.path,
    source: $query.source || "unknown"
};
```

Because the script does not explicitly send a response, its object return becomes a 200 JSON response.

## 3. Matching behavior

A script runs only after the normal data-plane policy chain:

1. Host selects a Project;
2. Host-level `X-Entry` and HTTPS policy;
3. Path matching;
4. optional exact method matching;
5. Project/Route CIDR and request-body policy;
6. execution of the precompiled program.

Path supports static segments, `:name` parameters, and a final `*name` wildcard. `$path.name` must refer to
a variable in the external Path; a typo rejects the entire candidate during Native Validator compilation.

A blank Method matches all. A configured Method is case-sensitive and byte-exact. Script Routes do not
support YAML `condition`, and a script cannot start executing and then decline in favor of a later Route.
Create separate same-Path Routes for method dispatch. Use a YAML condition for candidate fallback, or have
the script produce the complete response itself.

## 4. Response behavior

When Gzip response is enabled, access-server negotiates `Accept-Encoding` and wraps the script response
with the shared streaming response writer. It only transforms the fixed common MIME allowlist (for example
`text/plain`, `application/json`, and `application/xml`); short, already encoded, and other MIME responses
remain identity responses.

### 4.1 Implicit response

When no `resp.send*()` call commits a response:

| Script result                      | HTTP behavior                                        |
| ---------------------------------- | ---------------------------------------------------- |
| `return value;`                    | 200 with JSON-encoded value                          |
| `return null;`                     | 200 with JSON `null`                                 |
| `return undefined;`                | 200 with JSON `null`                                 |
| `return;` or fall off the end      | 204 with an empty body                               |
| Uncaught Exception / runtime Abort | 500 `SCRIPT_EXECUTION`, if no response was committed |

Use explicit responses for business APIs when status and content type should be obvious to readers.

### 4.2 Explicit JSON

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

`resp.sendJson(status, body)` sets `Content-Type: application/json` and ends the response.

### 4.3 Text, binary, and empty responses

```javascript
// Text; sets text/plain;charset=utf-8.
resp.send(200, "ready");
return;
```

```javascript
// Raw bytes; set Content-Type explicitly.
let bytes = binary.fromHex("89504e47");
resp.setHeader("Content-Type", "application/octet-stream");
resp.send(200, bytes);
return;
```

```javascript
resp.send(204);
return;
```

Send functions are asynchronous host functions, but the language has no `await`; the VM suspends and resumes
at the ordinary call. Return immediately after sending. Do not set headers again, send twice, or expect an
error after submission to change an already committed HTTP response.

## 5. Read the request

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

Constants avoid materializing a complete header/query/cookie object when only known fields are needed:

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

`$header`, `$cookie`, and `$context` fold ASCII case and normalize `-` with `_`. A query key that cannot be
written as an identifier should use `req.getQuery("key.with.dot")`.

### 5.2 JSON body

External Method: `POST`

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

`req.readJson()` buffers the complete body. Empty input, read failure, or invalid JSON throws a catchable
`Error`.

### 5.3 Binary body

```javascript
let payload = req.readBinary();
return {
    bytes: length(payload),
    sha256: hash.sha256(payload),
    base64: binary.base64Encode(payload)
};
```

An empty body returns a zero-length `Binary`. The complete body is made contiguous in request memory.

### 5.4 Discard the body

```javascript
req.discardBody();
resp.send(204);
return;
```

The body is a one-shot stream. Do not combine or repeat `readJson()`, `readBinary()`, and `discardBody()`.

## 6. Request-body boundary

Because script body functions buffer the full body, access-server applies this fail-closed rule before
SCRIPT execution:

- no body: allowed;
- valid Content-Length within the global server request limit: allowed;
- Content-Length over the limit: 413 `REQ_BODY_TOO_LARGE`;
- chunked or otherwise unknown-length body: 413 `REQ_BODY_TOO_LARGE`.

SCRIPT does not accept a Route-level `max_client_body_size` field, and the Console script editor does not
expose one. Use `PROXY` for large bodies or end-to-end streaming; do not read a body into a script to imitate
a proxy.

## 7. Headers and cookies

### 7.1 Response headers

```javascript
resp.setHeader("Content-Language", "en");
resp.addHeader("Vary", "Accept-Language");
resp.addHeader("Vary", "Accept-Encoding");
resp.sendJson(200, { ok: true });
return;
```

`setHeader` replaces a pending same-name value and `addHeader` appends. An empty name or empty converted value
throws `Error`. Protocol framing remains owned by `HttpExchange`; do not set `Content-Length`,
`Transfer-Encoding`, `Connection`, or other hop-by-hop/framing fields.

Access-server's HSTS and trace response fields are copied into the script response context before execution.
A script can set ordinary business fields, but must never echo Authorization, cookies, internal trace secrets,
or request/response bodies.

### 7.2 Cookie

```javascript
let added = resp.addCookie({
    name: "locale",
    value: "en",
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

`name` is required, `maxAge` must be an integer, boolean fields must be booleans, and `sameSite` accepts only
case-sensitive `Lax`, `Strict`, or `None`. An invalid object returns false rather than a detailed field error.

## 8. Error handling

Standard-library and HTTP semantic errors are catchable:

```javascript
try {
    let text = req.getHeader("X-JSON");
    return { payload: JSON.parse(text) };
} catch (error) {
    resp.sendJson(400, { error: "invalid payload" });
    return;
}
```

Return stable, non-sensitive client errors. Do not return the raw error object, script source, headers,
cookies, or bodies. An uncaught Exception or uncatchable runtime Abort maps to stable 500
`SCRIPT_EXECUTION` when the response has not already been committed.

Keep configuration and request failures distinct:

| Stage                    | Example                                                 | Result                                    |
| ------------------------ | ------------------------------------------------------- | ----------------------------------------- |
| Console local validation | Empty path, invalid method token, empty source          | Save blocked                              |
| Server compilation       | Invalid model shape, size, or wire compilation          | Version/Release blocked                   |
| Native Validator         | Syntax error, unknown function, missing `$path` capture | Candidate rejected; old snapshot retained |
| Request execution        | Invalid JSON body, type error, explicit `throw`         | Catchable, otherwise 500                  |
| Runtime Abort            | OOM, InvalidState, and similar failures                 | Not catchable; execution terminates       |

Validator errors should expose a stable code, Route field path, and bounded compile position, not full source.

## 9. Available and unavailable capabilities

Available:

- JS-like literals, variables, objects, arrays, operators, and control flow;
- array, strings, binary, hash, math, rand, JSON, Object, and URL standard libraries;
- request metadata/body through `req.*`;
- headers, cookies, and responses through `resp.*`;
- `$path`, `$query`, `$header`, `$cookie`, `$context`, `$req`, and `$conn`;
- registered synchronous and asynchronous host functions, without `await` syntax;
- configuration-time compilation and request-time execution of immutable programs.

Unavailable:

- Node.js, browser APIs, or a complete ECMAScript runtime;
- `fetch`, XMLHttpRequest, files, environment variables, sockets, timers, modules, or imports;
- Promise, `async`/`await`, user functions, or classes;
- `while`, classic three-part `for`, or regular expressions;
- dynamic object methods such as `items.push(x)`; use `array.push(items, x)`;
- outbound `directive backend = http "..."`, `http.request()`, `proxyPass()`, or dynamic upstream APIs;
- execution of real request scripts in the Console, control-plane server, or Native Validator.

The pinned Fiber library contains HTTP upstream directives that other hosts may enable, but Access Gateway
compiles Routes with `http_directives_enabled = false`. Use a YAML `PROXY` Route for upstream traffic so
service discovery, protected headers, body limits, observability, cancellation, and shutdown remain intact.

## 10. Recipes

### 10.1 Bounded diagnostics

External Path: `/_gateway/diagnostics`

External Method: `GET`

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

Do not return remote addresses, internal headers, or trace context from a public endpoint without explicit
authentication, authorization, and field review.

### 10.2 Stable canary bucket

```javascript
let user = req.getCookie("user_id") || req.getHeader("X-User-Id") || "anonymous";
let canary = rand.canary(5, user);

return {
    bucket: canary ? "canary" : "stable"
};
```

Keyed `rand.canary()` uses a stable CRC-32 bucket. It is not suitable for cryptography, tokens, or access
control, and a script cannot dynamically proxy based on it. Use data-plane cluster/gray policy for actual
traffic splitting.

### 10.3 Normalize a form query

```javascript
try {
    let parsed = URL.parseQuery(req.getQueryStr());
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

`URL.*` follows `application/x-www-form-urlencoded`: space encodes as `+`, and `+` decodes as space.

### 10.4 Content digest

```javascript
let body = req.readBinary();
resp.sendJson(200, {
    length: length(body),
    sha256: hash.sha256(body)
});
return;
```

Use only for small requests within the global body limit. MD5 and SHA-1 are for interoperability only;
prefer SHA-256 for a non-keyed digest.

## 11. Performance and security guidance

1. Narrow traffic with Path and method before entering a script.
2. Prefer `$header.x` and `$query.x` for known single values; materialize whole objects only when enumerating.
3. Avoid repeated JSON parse/stringify and repeated copies or spreads of large arrays and objects.
4. Bodies are fully buffered; keep them small and require Content-Length.
5. Return immediately after sending; translate expected input errors into stable 4xx responses.
6. Never create metrics labels from arbitrary Projects, Paths, headers, or user input.
7. Never log script source, Authorization, cookies, sensitive headers, or bodies.
8. Use `resp.sendJson()` for JSON and `URL.*` for form encoding; do not concatenate untrusted output.
9. Do not use hash or canary functions as authentication, authorization, or cryptographic randomness.
10. Run Native Validator before publication and expect the old snapshot to survive failure. After publication,
    require instance evidence before claiming activation.

See the script language, standard-library, and HTTP API reference for every signature and compatibility rule.
