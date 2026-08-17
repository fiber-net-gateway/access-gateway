# Access Gateway route rules and detailed usage

This guide is for developers and operators who configure Project routes in the Console. It describes
the Host, Path, Method, condition, CIDR, `RESPONSE`, `PROXY`, and publication behavior implemented by
this repository. The final authority is the wire codec, route compiler, and Native Validator in
`native/access-server`, not browser-side editor hints.

> Qualification: this guide describes the repository-owned C++ data plane and pinned Fiber revision
> `0fda7764bf94944aca4b674ab5ab311184703118`. Production script-corpus differential verification and
> the final cutover gates are not all complete. This guide does not claim production cutover readiness.

## 1. Core model

Only `access-server` receives or proxies gateway traffic. The Console edits, validates, versions, and
publishes configuration; it does not forward ordinary gateway requests.

An inbound request is processed in roughly this order:

1. pin the current immutable configuration snapshot for the entire request;
2. normalize `Host` and select a Project from the global Host tree;
3. apply Host-level `X-Entry` and HTTPS policy;
4. select Path candidates, then evaluate optional `method` and `condition` in order;
5. apply request-body and resolved client-address CIDR policy to the matched Route;
6. execute `RESPONSE`, `PROXY`, or `SCRIPT`;
7. emit bounded metrics, traces, and structured access logs.

The Route list is ordered, but list order only decides among candidates mounted at the same Path
matcher node. Across different shapes, the matcher first prefers static segments, then parameter
segments, then trailing wildcard segments.

## 2. Configuration and lifecycle

### 2.1 Keep the three states separate

| State                         | Meaning                                                          | Proves an instance is using it |
| ----------------------------- | ---------------------------------------------------------------- | ------------------------------ |
| Draft / configuration version | Immutable content saved in the Console database                  | No                             |
| Published                     | Content written to and read back from rnacos                     | No                             |
| Active on instance            | Typed activation evidence from a specific access-server instance | Yes                            |

A successful rnacos write or matching readback proves publication, not activation. When per-instance
evidence does not exist, the Console must show activation as unknown rather than rendering Published as
Active.

For a published Release, the Console reports `active` only when every required instance provides a
fresh exact MD5/version match. A not-yet-active candidate is `pending`, an explicit rejection or poll
failure is `degraded`, and missing or expired evidence is `unknown`. Instance details are loaded from
the Release page on demand.

### 2.2 Console editing model

A Project can contain both editor formats in one ordered list:

- YAML Route: one YAML mapping containing `path`, optional `method`, and execution fields. YAML Routes
  can only be `PROXY` or `RESPONSE`.
- JavaScript Route: the source contains only the script. `path` and optional `method` are separate fields
  above the editor. Publication compiles it to wire `type: SCRIPT`.

Each editor contains one Route; do not put a `routes:` array in a Route editor. Drag or keyboard reorder
changes candidate order. Saving creates a new immutable configuration version and never rewrites history.

The Console parses YAML 1.2 with the core schema and additionally requires:

- a mapping at the root;
- unique keys;
- no anchors, aliases, or explicit tags;
- only JSON-safe strings, booleans, arrays, objects, `null`, and safe integers;
- rejection of unknown fields instead of relying on the native compatibility codec to ignore them;
- at most 1 MiB per JavaScript source; YAML source and all source in one Project share the 4 MiB
  project payload budget; and at most 5000 Routes. Sizes are UTF-8 bytes, not browser character counts.

### 2.3 Native resource budgets

At startup the Console probes versioned limits from the Native Validator and displays the applicable
quota on the Routes and Network Policy pages. Common schema-version-1 boundaries include a 256 KiB / 1,024
entry Project List, a 4 MiB / 5,000 Route Project payload, 2,048-byte Paths, 64-byte Methods, 256 CIDRs or
static addresses per Route, 1 MiB scripts/templates, 2 MiB decoded static response bodies, 8 MiB aggregate
static response storage, and an approximately 64 MiB compiled-snapshot budget.

The browser only blocks limits it can determine directly. Saving, validation, and publication remain
authoritative in the server compiler and Native Validator. The stable over-limit code is
`limit_exceeded`. An over-limit rnacos candidate retains the instance's previous snapshot, and an
over-limit Project List does not unload current Projects. Release creation requires the Validator and
its probed limits to be available, but that still does not prove instance activation.

## 3. Host and Project selection

### 3.1 Host normalization

Host matching is ASCII case-insensitive. A port and one trailing dot are removed before matching.
Bracketed IPv6 literals retain the address while a following port is removed. Empty Hosts, consecutive
dots, slashes, control bytes, and other invalid values do not enter the matcher.

Supported patterns are:

| Pattern         | Example           | Match                                                             |
| --------------- | ----------------- | ----------------------------------------------------------------- |
| Exact           | `api.example.com` | Only that Host                                                    |
| Global wildcard | `*`               | Any valid Host                                                    |
| Suffix wildcard | `*.example.com`   | `a.example.com` and `a.b.example.com`, but not bare `example.com` |

Two Projects cannot declare Host patterns that normalize to the same value. The Console normally derives
one exact Host from the Project domain, but direct wire configuration must still respect global conflicts.

An unmatched Host returns HTTP 404 with error name `ROUTER_NOT_FOUND`. Path selection starts only after a
Host matches.

### 3.2 HTTPS and entry policy

A Host can leave HTTPS optional or redirect with status 301, 302, 307, or 308. A request is HTTPS when its
resolved external scheme is `https`. Otherwise the response is:

```text
Location: https://<effective-host><original-uri>
Strict-Transport-Security: max-age=31536000
```

After a Host matches, access-server prepares HSTS for subsequent responses. Valid Route
`response_headers` can override it. If an entry mask is configured, `X-Entry` must match an allowed
`vdi`, `desktop`, or `internet` entry; otherwise access-server returns 403 `ENTRY_ERROR`.

`ACCESS_SERVER_CLIENT_METADATA_MODE=direct` is the default: the scheme comes from the actual business
listener TLS state and all forwarding headers are ignored. `Forwarded: proto=` or an aligned
`X-Forwarded-Proto` is accepted only in `trusted_proxy` mode when the socket peer matches a configured trusted
proxy CIDR. The old trust-all behavior is available only through explicit `legacy_headers` mode.

## 4. Path patterns

Path patterns contain ASCII bytes and are split on `/`:

| Form              | Example         | Meaning                                          | Available to scripts/templates |
| ----------------- | --------------- | ------------------------------------------------ | ------------------------------ |
| Static            | `/users/me`     | Exact segment                                    | None                           |
| Parameter         | `/users/:id`    | Capture one segment; trailing `/` can bind empty | `$path.id`                     |
| Trailing wildcard | `/assets/*rest` | Capture the remaining path; must be last         | `$path.rest`                   |

For example, `/tenants/:tenant/files/*rest` matches
`/tenants/acme/files/a/b.txt` with `tenant = "acme"` and `rest = "a/b.txt"`.

Rules and limits:

- `*name` must be the last segment; `/assets/*rest/meta` rejects the candidate snapshot;
- one pattern cannot repeat a variable name, so `/:id/children/:id` is invalid;
- a static branch has priority over a parameter branch, which has priority over a wildcard branch;
- Path matching is case-sensitive;
- the matcher collapses repeated `/` and preserves Java-compatible trailing-slash behavior. For example,
  when only `/a/b/:value` exists, `/a/b/` matches with an empty `value`. Do not distinguish business Routes
  with repeated or trailing slashes; publish canonical patterns;
- the query string is not part of Path matching; use `$query.*` or `req.getQuery()`;
- `$path.name` must name a capture declared by the current pattern, or configuration compilation fails.

A method mismatch does not produce 405. If every Path candidate fails, the response is 404
`URL_NOT_MATCHED` and no implicit `Allow` header is added.

## 5. Method, condition, and ordering

### 5.1 Method

Omit `method` or set it to `null` to match every method. A configured value must be a 1–64 byte HTTP
token containing only:

```text
! # $ % & ' * + - . ^ _ ` | ~ DIGIT ALPHA
```

Matching is byte-for-byte and case-sensitive. Use conventional uppercase methods such as `GET` and
`POST`; `get` only matches an actual lowercase `get` token and is not normalized.

### 5.2 Condition

A YAML Route can include a synchronous `condition` expression:

```yaml
path: /reports/:id
method: GET
type: PROXY
condition: '$query.preview == "1" && $header.x_role == "reviewer"'
service: report-service/stable
```

`method` and `condition` use AND semantics. Method is compared first; only then are `$path` values bound
and the condition evaluated. The field contains an expression, not a `return` statement. Async functions
such as `req.readJson()` reject configuration compilation. A falsey result, uncaught exception, or failed
condition evaluation skips that candidate and continues with later candidates at the same node.

### 5.3 Candidate order at one path

This order is valid:

1. `GET /items/:id` with a more specific condition;
2. `GET /items/:id` without a condition;
3. `POST /items/:id`;
4. an all-method `/items/:id` fallback.

The same Path can have different methods or conditions. An identical predicate is rejected. A Route with
neither method nor condition is the unconditional terminal candidate for that node; another Route after
it is dead and rejects the entire candidate snapshot.

Put a same-Path fallback last. Reordering static, parameter, and wildcard Routes does not override the
matcher's structural priority.

## 6. YAML Route field reference

| Field                  | Route types    | Rule and default                                                         |
| ---------------------- | -------------- | ------------------------------------------------------------------------ |
| `path`                 | All            | Required, non-empty ASCII Path pattern                                   |
| `method`               | All            | Optional; missing/`null` matches all, otherwise exact                    |
| `type`                 | All            | YAML must use `RESPONSE` or `PROXY`                                      |
| `condition`            | YAML           | Optional synchronous expression                                          |
| `service`              | PROXY          | Named service, optionally `service/cluster`                              |
| `cluster`              | PROXY          | Explicit cluster overriding the service suffix                           |
| `addresses`            | PROXY          | Static HTTP(S) authorities used when `service` is absent                 |
| `proxy_headers`        | PROXY          | Header templates sent upstream                                           |
| `response_headers`     | RESPONSE/PROXY | Header templates sent to the client                                      |
| `context`              | PROXY          | Trace/CAT context templates                                              |
| `rewrite`              | PROXY          | Upstream path template; original query is appended                       |
| `status`               | RESPONSE       | Required; native accepts 100–999; use standard HTTP statuses             |
| `body`                 | RESPONSE       | Optional; missing means an empty body                                    |
| `gzip`                 | RESPONSE       | Optional; `true` uses level 6, `1`–`9` selects a level, `false` disables |
| `timeout`              | PROXY          | Response-header timeout; default 60000 ms, minimum 5 ms                  |
| `max_client_body_size` | RESPONSE/PROXY | Request limit; missing or numeric 0 uses the server default              |
| `max_proxy_body_size`  | PROXY          | Non-zero value overrides the upstream-response limit                     |
| `websocket_timeout`    | PROXY          | Positive value enables WebSocket tunneling and sets tunnel I/O timeout   |
| `flush`                | PROXY          | `true` reduces local buffering and adds `X-Accel-Buffering: no`          |
| `allows`               | All            | CIDR allow/deny sequence; `!` prefixes deny entries                      |

Durations accept integer milliseconds or case-insensitive strings such as `500`, `500ms`, and `5s`.
Data sizes accept integer bytes or binary units such as `64k`, `4m`, and `1g`. New configuration should
use clear, non-negative values and must not depend on legacy Java overflow or negative-value behavior.

Header and `context` mapping values must be scalar or `null`; the Console deterministically compiles them
to wire scalars. Never place credentials, cookies, Authorization values, bodies, or other secrets in
documentation, change summaries, audit details, or logs.

## 7. RESPONSE Routes

### 7.1 Text

```yaml
path: /healthz
method: GET
type: RESPONSE
status: 200
body:
    type: TEXT
    content: ok
gzip: true
response_headers:
    Content-Type: text/plain; charset=utf-8
    Cache-Control: no-store
```

`TEXT` content must be non-empty and is sent as UTF-8 bytes.

### 7.2 Base64

```yaml
path: /pixel.gif
method: GET
type: RESPONSE
status: 200
body:
    type: BASE64
    content: R0lGODlhAQABAAAAACw=
response_headers:
    Content-Type: image/gif
```

`BASE64` is decoded while compiling configuration. Invalid characters or padding reject the candidate
snapshot rather than failing on the first request.

### 7.3 Template

```yaml
path: /hello/:name
method: GET
type: RESPONSE
status: 200
body:
    type: TEMPLATE
    content: '{"message":"hello ${$path.name}"}'
response_headers:
    Content-Type: application/json; charset=utf-8
    X-Request-Method: "${$req.method}"
```

Templates concatenate text; they do not automatically apply JSON, HTML, or URL escaping. Encode untrusted
input at the output boundary. For nontrivial JSON, prefer a JavaScript Route with `resp.sendJson()`.

Each `${expression}` is compiled during configuration loading and evaluated synchronously per request.
Template text supports `\\`, `\$`, `\{`, and `\}` escapes. `null`, `undefined`, objects, and arrays render
as empty text; scalar values use compatibility text conversion.

A RESPONSE drains the request body, then atomically prepares all Route response headers and the body. If
any template or header validation fails, no partial Route output is committed and stable error handling
takes over.

The Route cannot override these hop-by-hop/framing response headers:

```text
Connection, Content-Length, Proxy-Connection, Keep-Alive,
Proxy-Authenticate, Proxy-Authorization, TE, Trailer,
Transfer-Encoding, Upgrade
```

### 7.4 Gzip content negotiation

Omitting `gzip` or setting it to `false` disables compression. `true` uses the default compression level
6; an integer from `1` through `9` selects that level. The first implementation supports only non-empty
`TEXT` and `BASE64` bodies. access-server freezes both identity and gzip bytes before publishing the
candidate snapshot, so the request hot path selects an existing representation without compressing it.
`TEMPLATE` bodies do not support gzip yet.

When enabled, access-server negotiates `gzip`, `identity`, `*`, and `q` weights from `Accept-Encoding`.
A request without that header receives identity; explicitly rejecting both available representations
returns 406 `NOT_ACCEPTABLE`. Every negotiated response merges `Vary: Accept-Encoding`; a selected gzip
variant carries `Content-Encoding: gzip` and its own `Content-Length`. A configured strong ETag is
weakened. HEAD performs the same coding selection and sends its headers without a body.

Configuration compilation rejects `gzip` on a PROXY Route, an empty or `TEMPLATE` body, a
1xx/204/205/206/304 status, or a simultaneous `response_headers.Content-Encoding` value.

## 8. PROXY Routes

### 8.1 NamingService upstream

```yaml
path: /api/orders/:id
method: GET
type: PROXY
service: orders/stable
timeout: 30s
rewrite: "/internal/orders/${$path.id}"
proxy_headers:
    X-Tenant: "${$header.x_tenant}"
response_headers:
    Cache-Control: no-store
```

`service: orders/stable` means service `orders` with default cluster `stable`. A separate non-empty
`cluster` overrides the suffix. NamingService selects only enabled, healthy, positive-weight instances.
No eligible instance produces a stable 503 error; traffic is never handed to the Console.

### 8.2 Static upstream

```yaml
path: /legacy/*rest
type: PROXY
addresses:
    - http://127.0.0.1:8080
    - https://legacy.internal:8443
rewrite: "/${$path.rest}"
timeout: 20s
```

A non-empty `service` takes precedence; `addresses` is not its failure fallback. Omit `service` to use
static addresses. Without a scheme, port 443 implies HTTPS and other ports imply HTTP. Explicitly specify
`http://` or `https://` to avoid port-dependent meaning.

Outbound HTTPS trust is process-wide: `ACCESS_SERVER_UPSTREAM_TLS_MODE` is `legacy_insecure` by default,
or can be set to `system_ca` or `custom_ca` (with `ACCESS_SERVER_UPSTREAM_TLS_CA_FILE`). Secure modes verify
the endpoint hostname or IP identity, and an invalid trust store prevents startup. Route-specific CA, SNI,
and verification-name overrides are not currently accepted because the pinned connection pool cannot yet
isolate connections by TLS transport profile. Changing CA bundle contents requires a rolling restart so
existing connections established under the previous trust store are drained.

### 8.3 Request and response forwarding

- the original method is preserved;
- without `rewrite`, the raw URI and percent-encoding are preserved;
- `rewrite` produces the path; an empty result becomes `/`, then the original query is appended;
- the downstream body streams upstream and never passes through the Console/control plane;
- `proxy_headers` overrides or suppresses a same-name inbound header; an empty template writes nothing;
- fixed hop-by-hop/framing headers and inbound `Host` are not copied directly; target authority supplies Host;
- access-server finally forces `x-ploto-source-app` to `<project>.unifiedAccess`;
- upstream status is preserved and response hop-by-hop fields are filtered;
- `response_headers` can override ordinary upstream fields; explicit Location/Refresh disables auto-rewrite;
- the upstream body is bounded by `max_proxy_body_size` or the server default.

A connection failure can select another endpoint only before the upstream request header starts, for at
most four selections including the first. Once header transmission begins, no retry occurs, preventing
duplicate non-idempotent submissions.

### 8.4 Flush and streaming

Use `flush: true` for SSE, streaming JSON, and other low-latency chunked responses. It reduces local body
aggregation and adds `X-Accel-Buffering: no`. It does not disable buffering in a CDN, Ingress, browser, or
protocol stack outside access-server; validate the complete path separately.

### 8.5 WebSocket

```yaml
path: /socket
type: PROXY
service: realtime/stable
websocket_timeout: 300s
```

The Java-compatible HTTP/1.1 path requires `websocket_timeout > 0`, `Upgrade` case-insensitively equal to
`websocket`, and `Connection` case-insensitively but exactly equal to `upgrade`. A comma-separated token
list does not satisfy this legacy check. After a successful 101 handshake, the connection becomes a
bidirectional tunnel and the timeout applies to tunnel I/O.

## 9. CIDR and body policy

### 9.1 Route CIDR

```yaml
path: /admin/*rest
type: PROXY
service: admin/stable
allows:
    - 10.0.0.0/8
    - 2001:db8::/32
    - "!10.20.0.0/16"
```

Entries without `!` form the allow set; prefixed entries form the deny set:

1. if the allow set is non-empty, the source must match at least one allow;
2. any deny match rejects the source;
3. deny therefore wins in the final result.

The source comes from the unified client metadata. `direct` mode uses the socket peer. After a socket peer
matches `ACCESS_SERVER_TRUSTED_PROXY_CIDRS`, `trusted_proxy` mode considers `Forwarded`,
`X-Forwarded-For`, then `X-Real-Ip`, and walks the chain right-to-left across trusted hops. Invalid or oversized
chains fall back to the socket peer, so safe modes never skip CIDR checks because a header is absent or invalid.
Only explicit `legacy_headers` mode preserves the Java rule that a missing or unparsable `X-Real-Ip` skips the
check. Address chains are limited to 32 hops. Each `Forwarded` element is limited to 16 unique parameters, and
extension values must also be valid tokens or quoted strings. Rejected requests return 403 `NOT_ALLOW_IP`.

When Console Network Policy makes Project policy authoritative, the compiler injects one allow/deny list
into every wire Route. Route YAML must not also define `allows`, or validation reports a policy conflict.

### 9.2 Request-body limits

The server startup setting supplies the default request-body limit. A non-zero Route
`max_client_body_size` overrides it. Exceeding the effective limit returns 413 `REQ_BODY_TOO_LARGE`.
PROXY enforces limits for both Content-Length and streaming bodies.

JavaScript Routes currently accept only no body or a valid Content-Length not exceeding the global
request limit. An unknown-length chunked/streaming body fails closed with 413 before script execution.
See the script-route guide for the full boundary.

## 10. Variables available to templates and conditions

These constants are resolved at configuration time and read at request time:

```text
$path.<route-variable>
$query.<query-name>
$header.<normalized-header-name>
$cookie.<normalized-cookie-name>
$context.<context-name>

$req.uri        $req.method      $req.path       $req.query
$conn.remote_addr  $conn.remote_port  $conn.http_version  $conn.scheme  $conn.tls
```

`$header`, `$cookie`, and `$context` fold ASCII case and normalize `-` with `_`. For example,
`$header.x_forwarded_for` reads `X-Forwarded-For`. Query or cookie names that are not valid identifiers
must use `req.getQuery("...")` or `req.getCookie("...")`.

Conditions and templates expose the synchronous standard library and synchronous request metadata only.
`resp.*` is unavailable, and async body functions are rejected. See the script language and API reference
for the complete list.

## 11. rnacos wire contract

Preserve these default locations:

| Purpose               | Data ID                                | Group           | Content                              |
| --------------------- | -------------------------------------- | --------------- | ------------------------------------ |
| Project list          | `ploto.unified-access.projects`        | `ACCESS-SERVER` | Semicolon-separated string, not JSON |
| Project Route         | `ploto.unified-access.route.<project>` | `ACCESS-SERVER` | Project JSON                         |
| Production gray rules | `ploto.unified-access.gray-match`      | `DEFAULT_GROUP` | Gray JSON                            |

A simplified Project payload is:

```json
{
    "version": 42,
    "host": {
        "api.example.com": {
            "https": "S_NOT_MUST"
        }
    },
    "routes": [
        {
            "path": "/healthz",
            "method": "GET",
            "type": "RESPONSE",
            "status": 200,
            "body": { "type": "TEXT", "content": "ok" }
        }
    ]
}
```

Do not publish the Console's normalized model directly. The publisher must deterministically compile the
compatibility format, and the Project list must remain a semicolon-separated string.

## 12. Update, failure, and rollback semantics

- a candidate with the same Project `version` as the active snapshot is ignored;
- a non-empty candidate is fully decoded and has its Path matcher, CIDRs, templates, scripts, and upstream
  dependencies prepared before atomic publication;
- any failure preserves the last successful snapshot; requests never observe a partial model;
- empty Project route content preserves current configuration and is not deletion;
- missing or empty `host` unloads that Project's Host and Routes;
- removing a Project from the Project list cancels its subscription and unloads it, which is distinct from
  a failed update;
- rollback creates a new Release from historical content and never rewrites Release history;
- every rnacos resource write, readback, failure, and later instance evidence is recorded separately because
  rnacos does not provide a multi-resource transaction.

## 13. Common failures

| HTTP / error               | Common cause                                          | Check first                                      |
| -------------------------- | ----------------------------------------------------- | ------------------------------------------------ |
| 404 `ROUTER_NOT_FOUND`     | Invalid, missing, or unmatched Host                   | Actual Host/port, Project domain, wildcard       |
| 404 `URL_NOT_MATCHED`      | Every Path/method/condition failed                    | Pattern shape, case, order, condition values     |
| 403 `ENTRY_ERROR`          | `X-Entry` not allowed by Host policy                  | Trusted frontend value and network policy        |
| 403 `NOT_ALLOW_IP`         | Resolved client address rejected by CIDR              | Metadata mode, trusted chain, allow/deny, family |
| 413 `REQ_BODY_TOO_LARGE`   | Route/global body limit or unknown-length SCRIPT body | Content-Length, transfer encoding, limits        |
| 500 template/script error  | Expression failure or uncaught script error           | Native field path, arguments, `$path` names      |
| 503 no hosts/circuit break | No eligible NamingService endpoint                    | service, cluster, enabled/healthy/weight         |

During an update, distinguish Draft, Published, and instance Active before diagnosing traffic. An old
snapshot continuing to serve does not mean an invalid candidate became active, and rnacos readback success
does not prove activation.

## 14. Recommended practices

1. Order the most specific method/condition candidates before an all-method fallback at the same Path.
2. Give static, parameter, and wildcard patterns clear responsibilities; avoid subtle shadowing.
3. For public APIs, explicitly set method, body limit, timeout, and trusted-source CIDRs.
4. Put scheme and port in static addresses; name service and cluster clearly for NamingService Routes.
5. Use RESPONSE for fixed output, PROXY for ordinary forwarding, and SCRIPT only for request-time computation.
6. Keep conditions and templates short, synchronous, and side-effect free; move complex logic upstream.
7. Never expose credentials, cookies, Authorization, sensitive headers, or bodies in templates, errors, or logs.
8. Run Native Validator before publication and wait for real instance evidence afterward; unknown remains unknown.
9. Compatibility claims require both native and Console validation plus the documented differential/cutover gates,
   not unit tests alone.
