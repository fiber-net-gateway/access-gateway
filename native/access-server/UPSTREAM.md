# Upstream provenance

The initial `native/access-server` source was imported from
`fiber-net-gateway/fiber-gateway-cpp/apps/access-server` at revision
`0fda7764bf94944aca4b674ab5ab311184703118`.

The application is maintained in this repository after the import. The reusable Fiber runtime,
HTTP, JSON/script, Nacos, CAT, and Prometheus modules are consumed from the pinned
`third_party/fiber-gateway-cpp` submodule. Updating that submodule gitlink is independent of this
historical application import revision.

The current reusable Fiber dependency is pinned at
`47e71f6cb34b01d20ff866dc3c274be16a368fe1` (upstream `master`, fetched 2026-10-19). The pinned
revision carries no repository-owned compatibility patches: the TLS dynamic-credential lifetime
defect ([upstream issue #41](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/41)),
briefly patched against `f832d5458d3bfca1cefa45a4c7a6cb1cd0662f23`, is fixed upstream in `90ed153`
(see `native/patches/README.md` for the retired-patch map).
The repository-owned regression
`ProxyExecutorTest.StreamsChunkedUpstreamWhoseFramingArrivesSeparatelyFromPayload` still guards
the HTTP/1 chunked split-framing fix from the application side.
The reviewed update range from the previous pin is
`512ef8a73a653d951ea80d9f3ccc095545577319..47e71f6cb34b01d20ff866dc3c274be16a368fe1`. Its
behavioral core is a pair of HTTP/1 data-plane fixes. The header parse buffer now tracks view
lifetime per storage (`193d189f`): a head larger than the initial arena that forced a grow freed
the storage still backing parsed header/method/URI views — a use-after-free reachable from one
crafted request — and a line ending exactly at the arena boundary aborted the process at a
non-NDEBUG assertion (deterministic remote DoS); the removed `retain_prefix` API is replaced by
`note_view`/`pin_viewed_storage`, both unused by access-server. The post-response unread-body
drain on HTTP/1 server connections is now bounded by `header_timeout` instead of waiting forever
(`73d22ebf`) — the exact window access-server's early-response paths (route-level 404/502, script
errors) leave open on a stalled client. The script engine eliminates its int64 UB with Java-parity
semantics (`2a778af0`: `INT64_MIN % -1` SIGFPE-killed the process from a one-line config script;
`math.floor`/`substring`/`repeat` now saturate like Java's `(long)double` through the new
`script/Numeric.h` helper) and bounds `http.request` response bodies at `maxResponseSize`
(default 8 MiB) plus a 16-run informational-response cap, both surfaced as catchable script
errors (`a6cbe76e`). QUIC stream teardown now derives its terminal `IoErr` from the close source
(`ba4cb2e6`, `cae1351a`: peer close/stateless reset → ConnReset, idle timeout → TimedOut, local →
Canceled) so parked and late IO report one label; access-server's canceled/failed classification
keys on its own `when_any` ordering rather than these codes, so only the error codes carried in
stage events become more accurate. The h3 client no longer aborts the whole request when a write
fails on a peer STOP_SENDING (`47e71f6c`): writes reject with BrokenPipe while the response stays
readable — the upstream half of the fix behind this repo's `Http3EarlyResponseCancelTest` flake
(the write path previously discarded an already-arrived head). The residual ~6% race was in this
repo's test, which tore the exchange down before its detached reader was joined; the test now
joins the reader with a `WaitGroup` before the exchange leaves scope (300 consecutive runs green;
the pre-fix pin flaked ~15/100, the intermediate pin ~6-7/100). The range also lands a TLS
reassembly fix for handshake headers split across records (`3ec85182`), Prometheus dynamic
series with strict family LRU behind an additive registry API (PR #44, `cafbb05b`; access-server
uses plain series), response-trailer ordering assertions (`bf4efc4e`), QUIC crypto-input
buffer-chain perf (`0689d1af`), and QUIC congestion/teardown test coverage. Public headers move
+80/−5 with the one removal unused here; `QuicStream::close`/`QuicStreamRecvQueue::stop_receiving`
gained defaulted terminal-error parameters. Access-server needed no source adaptation for this
update.
The reviewed update range from the previous pin is
`feb9ae15600b5293623d6db3337db2066d595d23..512ef8a73a653d951ea80d9f3ccc095545577319`. Its
behavioral core is the HTTP/3 early-response cancellation code change
(`fix(http3): stop unread request bodies with H3_NO_ERROR`): when a handler finishes with a
complete response and an unread request body remains, the STOP_SENDING now carries
`H3_NO_ERROR` (256) instead of `H3_REQUEST_CANCELLED` (268), per RFC 9114 §4.1 semantics of
preserving the complete response; the aborted-response path still resets the send direction with
`H3_REQUEST_CANCELLED`. The range also lands QUIC transport fixes (Version Negotiation parsing
without the fixed bit, in-flight packet tracking across congestion resets, handshake failure on
out-of-order CRYPTO below an already-fed level, rejection of a QUIC ServerHello without
supported_versions), QUIC performance rework (crypto input through buffer chains, connection-ID
hash caching, intrusive output-frame lists with IoBuf-owned payloads), script GC heap
statistics, and a repo-wide clang-tidy rollout (infrastructure plus per-module batches; no
intended behavior change). Interop verification on this pin: Chrome QUIC receives
STOP_SENDING(256) and resets with 256 within ~3ms, but the fetch/XHR hang itself persists —
Chrome never surfaces the early response while the reset upload is unfinished, independent of
the app error code. Access-server needed no source adaptation.
The previous reviewed range from its predecessor pin is
`f2e330bc16fbb6cf7499b824de9931b0ece60a13..feb9ae15600b5293623d6db3337db2066d595d23`. It fixes
the CAT client's write-only collector transport — the root cause of the 2026-10-02 CAT blackout
on 172.28.2.111, where a collector disconnect under benchmark load went unnoticed for hours
while every local counter stayed green. Collector connections now keep read and terminal
callbacks armed even with an empty send queue: peer EOF (including a write-half-close), read
errors, and terminal events fail the connection and wake the reconnect controller, which
reconnects with exponential backoff that only resets after a connection survives
`reconnect_max_delay` (a collector that accepts and immediately closes can no longer keep the
client on an immediate-retry loop). Unsolicited collector bytes are drained with a fixed
per-turn read budget; intentional shutdown and Router-driven switches detach the callbacks
before closing and do not count as failures. New counters `connection_failures`,
`read_failures`, and `peer_closes` land in `CatClientStats` and the heartbeat detail
(access-server does not yet export them to `/metrics`). Known boundary, recorded upstream: the
subscriptions detect peer closure, not a silently forwarding black hole; periodic connection
rotation remains a possible future hardening. Access-server needed no source adaptation — the
change is internal to the CAT module and additive to its stats.
The previous reviewed range from its predecessor pin is
`fab35402f3a5ddf4b78ade82d187a9d89972b244..f2e330bc16fbb6cf7499b824de9931b0ece60a13`. It lands
the HTTP/2 send-lifetime fix for the coroutine-destruction assert on in-flight response-body
writes (`0f71cc77`, with its root-cause report) and then rebuilds the H2 outbound path on top of
it: sends are queued from the stream when an operation arms (`b3c35ebd`), reduced to shared
frame-encoder operations with DATA sends shared per connection (`6cdf3fad`, `aa3db2a8`),
withdrawn when close abandons them (`02e17bde`), encoded on the pump into the in-flight chain
(`9f832500`) with the DATA end flag kept past encoding (`c213c158`), H2 I/O is driven from
transport readiness (`760f320f`) and inbound is pumped before outbound (`f2e330bc`). The range
also reworks the TLS data path: `TlsStreamFd` seals batches of records per write (`8b95cae0`,
`72f49378`) and TLS 1.3 records without staging the plaintext (`be3f3097`), frames connected-phase
records off one contiguous wire buffer and opens them in place (`c55b263`, `a4612e5d`), counts
buffered plaintext as read readiness (`b41a6e3c`), releases unflushed output when closing
(`b34e2b0d`), and drains its sealed output on its own (`03e290ee`) — the write side becomes
self-draining, so no transport keeps a reference to a caller's chain past `try_writev` and
`abandon_pending_io` disappears. `TlsCredential` becomes a refcounted handle with copy/move
semantics (`f76f3f68`); `TlsServerHandshakeConfig::add_credential(TlsCredential)` takes the
handle by value (the handshake holds its own reference — the replacement for the removed
`shared_ptr` owning overload) and `add_borrowed_credential(const &)` is the explicit borrow.
Handshake engine state is held by value (`90ea37b4`, `23a73445`), the handshake-phase split by
role (`06c8569f`), and the engine headers leave the public include tree. The readiness API
folds: `has_pending_read`/`has_pending_write` are removed in favor of pure-virtual
`read_ready()`/`write_ready()` on `HttpTransport` (`1e57f2fd`, `4efc4af2`). QUIC stops using a
peer stream the attach callback retired (`d4bdde31`) and releases the handshake engine after the
handshake (`64d163fc`); dead `ServerHttp2Push` is deleted (`645899ea`).
Access-server adapted to the handle model: `UpstreamTlsClientIdentity` holds its
`net::TlsCredential` by value instead of `unique_ptr`, and `TlsCertificateStore` (plus the
rotation test callback) stages the selected credential via `add_credential(*selected)` — the
handshake keeps the material alive past snapshot reclamation exactly as the aliasing
`shared_ptr` overload did, while the snapshot itself remains reclaimable. Static-credential
tests pass `&*credential` to `configure_ctx`. The suspended-handshake rotation regression no
longer includes the engine header that left the public tree; it encodes its ClientHello through
the public codec (`tls_encode_client_hello` + `TlsKeyExchange` + a hand-built plaintext record)
and observes material lifetime through the kept handle's `use_count()` instead of an aliasing
`weak_ptr`.
The previous reviewed range from its predecessor pin is
`270035b7707b8d3825e31eca2d1c67d1da1ef640..fab35402f3a5ddf4b78ade82d187a9d89972b244`. It adds
mutable request-phase accessors on `HttpExchange` (non-const `request_headers()`,
`request_header_refs()`, `uri()`) with the documented contract — pool-copying `set()`/`add()`
only, protocol-control names frozen, first-wins refs maintained by the caller — which
access-server uses to rewrite the Host header to the cluster-stripped form in test mode, and
caches an `accept-encoding` request-header ref used by gzip negotiation as an absent
short-circuit.
The complete reviewed range from the original import pin is
`0fda7764bf94944aca4b674ab5ab311184703118..270035b7707b8d3825e31eca2d1c67d1da1ef640`.
It removes the obsolete upstream `apps/access-server`, adds Nacos hostname and bounded service
status APIs, system resolver/multi-nameserver support, client TLS identities and HTTP/1 pool
affinity, a cancellable Happy Eyeballs connector, and a reusable public HTTP gzip response writer
shared by lite-nginx and repository-owned access-server. The script exchange now permits a request
local response-writer decorator. The complete range also validates and scans DNS record sections
without materializing unrelated records beyond the configured retention limit, coalesces
overlapping Prometheus collections onto one stable shard-snapshot generation, and moves
Fiber-internal HTTP, QUIC, DNS, and script implementation headers out of the public include tree.
An earlier reviewed delta splits TLS context material (certificate chain, private key, trust store)
from per-connection policy: `TlsContext` becomes role-neutral and is created by a synchronous
factory, connection policy moves to `TlsClientConnectionOptions`/`TlsServerConnectionOptions`, and
SSL objects are created from handshake parameters with credential references held for the
connection lifetime. Access-server was migrated to the new TLS APIs in the same change; no wire
contract changes were required. The delta also replaces the script engine's libm `pow`/`fmod` uses
with self-contained correctly-rounded equivalents so `fiber_lib` references no libm symbol newer
than GLIBC_2.2.5. The latest reviewed delta moves TLS policy out of the TLS parameter objects and
into the transports that drive handshakes: `TlsClientParam`/`TlsServerParam` become borrowed
handshake-time views (client identity material under `TlsClientSecurity`, `string_view` names,
span-based ALPN, no handshake timeout), `HttpServerOptions` moves to its own public header with
`tls` now an `HttpServerTlsOptions` value, and HTTP/1 client connections take an optional
`HttpClientTlsOptions` instead of an always-present `TlsClientParam`. Servers and clients now
advertise their own fixed ALPN protocol sets, `TlsTransport` receives its TLS param at
`handshake()` rather than `create()`, and `TlsTransportKind`, `TlsNewSessionOps`, the ClientHello
address/transport fields, and `TlsServerHandshakeConfig::select_alpn` are removed. The tip commit
adds a process-wide `TrustStore::system_default()` cache used to fall back to system trust roots
when verifying clients. The latest reviewed delta rewrites the HTTP server stack around a
`Server`/`Endpoint` lifecycle: endpoints (`Http1Endpoint`, `Http2Endpoint` with optional HTTP/1
negotiation over TLS, `Http3Endpoint` sharing the TCP listener's port with server-side GOAWAY
drain) are staged on a `Server` before one `start()` and torn down via `stop()`/`stop_and_wait()`,
and the legacy `HttpServer`/`Http3Server` trio is deleted. On the client side it adds multiplexed
HTTP/2 connection pools with a reuse fast path and surfaced dial failures, a protocol-agnostic
`ClientHttpExchange` entry point, and per-`connect()` client parameters: connect options move out
of connection state, `HttpClientTlsOptions` names become borrowed `string_view`s, and the shared
pool grouping components are renamed `HttpConnectionGroupKey`/`HttpConnectionPoolAffinity`.
HTTP/2 server connections are sharded into per-loop workers with keepalive collapsed to one
`read_timeout` and idle h2 server connections retired. Access-server was adapted to the rewritten
server lifecycle, per-call connect parameters, and renames in the same change (runtime listener
staging via `http::Server` + typed endpoints, proxy upstream dialing, and test/benchmark fixtures);
no wire contract changes were required. The latest reviewed delta (from pin
`dfa5676c0a4e186767372ea5d2e1dd5573ba925a`) lands the four defects formerly patched under
`native/patches/` directly upstream with their root-cause reports and Fiber-side regression
tests: HTTP/1 chunked bodies whose framing spans transport reads (`09dd0ef`), the HTTP/3
terminal FIN stranded behind inflight body extents — including a superset that re-queues
stranded send work on stream ACKs and keepalive backlog (`3ccecc4`) — the HTTP/2 terminal-only
body write not consuming the borrowed chain's completion marker (`d968408`), and QUIC awaiter
resumes made retraction-safe against hard coroutine destruction by moving them to the
cancellable local defer queue (`1e30624`). The delta also scopes the h2 drain test's options
and adds an all-protocol benchmark retest. Access-server needed no source adaptation for this
update: every change is either internal to the pinned modules or additive
(`ChunkedBodyParser::payload_remaining`). The latest reviewed delta (from pin
`e9a804053b14c206ed4a4fcd3b89e9a6789387a2`) hardens the QUIC and HTTP gate machinery and
splits the HTTP/3 connection along its roles. Idle HTTP/3 server sessions are retired with a
GOAWAY after a new default-off `Http3ServerOptions::idle_connection_timeout` — a policy
distinct from QUIC liveness, since a session always holds four long-lived control streams, so
only the HTTP/3 layer can tell it has no work — and the QUIC keepalive interval is capped at
half the negotiated idle timeout in both directions. Both stream gates fix the same departure
defect: a waiter signaled with credit or capacity that departs before its posted resume runs
now frees its slot through `detach_waiter()`, waking the remainder instead of letting
successors sleep to their deadlines (QUIC stream credit and HTTP/2 capacity alike), awaiter
completion becomes irreversible so a completed waiter can never be parked again, and the HTTP
connection registries read the next node before the drain callback, so an HTTP/1 connection
whose `request_drain()` tears the session down synchronously can no longer leave the shutdown
traversal reading freed memory. QUIC connection state is funneled through one
`transition_state` (two close paths previously forgot to wake handshake waiters, parking
coroutines until their own timeout or a destructor assertion), the handshake wait queue moves
into `QuicHandshakeGate`, local-stream admission moves out of `QuicConnection` into
`QuicLocalStreamGate` behind a pure attach-status query (`None`/`Busy`/`Canceled`) with new
connection Ops hooks (`on_active_request_count_change`, and `on_capacity_change` widened to
stream attach and detach in both directions), idle-timeout closes go through `enter_closed()`,
and state observers dispatch after transition bookkeeping. In the core, `IntrusiveList`
becomes a sentinel-anchored self-unlinking ring whose hooks unlink themselves on destruction,
the five hand-kept park/resume awaiters consolidate into one `WaitAwaiter` primitive, and the
shared `Http3Connection` class is deleted in favor of the server owner and client impl
embedding one private `Http3ControlStreams` component. The delta also hardens the upstream
test harness (detached pool handlers and the idle h2 client close watcher are awaited before
teardown) and records the drain use-after-free and an ASan sweep in its defect-report docs.
Access-server needed no source adaptation for this update: every change is internal to the
pinned modules or additive, and the only public-header addition it does not yet configure is
the default-off idle retirement option. The latest reviewed delta (from pin
`7e6930fc6b432c9f5575d969d14d249a2587dc0d`) flips that default on:
`Http3ServerOptions::idle_connection_timeout` now defaults to 70 seconds, so sessions sitting
with no request running are retired with a GOAWAY out of the box (zero still disables it),
matching the HTTP/2 endpoint's idle retirement. Access-server stages its HTTP/3 endpoint
without configuring the option, so idle HTTP/3 sessions are retired after 70 seconds by
default; no source adaptation was needed. The latest reviewed delta (from pin
`7da6926ee410fcd6ccdd9b50c44c5638cbc61eb8`) rewrites `HttpConnectionGroupKey` around a single
`make(host, port, scheme, optional pinned IP)` factory — `from_name`/`from_ip`/`HostKind` are
gone, and an IP literal combined with Https is rejected because SNI cannot carry a literal
(RFC 6066 §3). Access-server adapted in three product decisions: static https upstreams that
resolve to an address literal compile to a pinned dial under a synthetic DNS-shaped name
(`<ip-with-dashes>.<port>.ip.invalid`, RFC 2606 reserved TLD) with the literal address pinned
into the key; Nacos instances registered as a 443 address literal dial pinned under the
sanitized service name (instances with no DNS-valid name are skipped and counted by the new
`invalid_upstream` discovery event); and script URL targets build the key from the URL host.
A pinned HTTPS dial (either form) carries no name context to authenticate the peer with, so
the handshake sends no SNI and skips peer verification — Java-gateway parity, recorded here as
an accepted security trade-off (named https upstreams keep their verification modes:
default LEGACY_INSECURE, optional SYSTEM_CA/CUSTOM_CA). The delta also removes
`HttpConnectionPoolAffinity`/`UpstreamTlsTransportProfile` pool isolation: connections are now
pooled under the base key, so during the window where a pool still holds leases created under
an older TLS profile generation, requests may reuse a connection negotiated with the previous
TLS parameters. The window is bounded by pool draining and self-heals on the next dial; an
upstream follow-up is planned to restore a discriminator. zlib 1.3.2's deflate core moves
in-tree (`src/compression/`, compiled into `fiber_lib`, with the streaming
`fiber/compression/GzipEncoder.h` public header), removing
`fiber_prepare_zlib_target()`/`FIBER_ZLIB_SOURCE_DIR`/`ZLIB::ZLIBSTATIC`; repository gzip
encoding now uses the streaming encoder and tests verify round-trips through the
`FiberZlibReference` reference objects. `http_script::HttpUpstreamConnection` gains a
`host_header()` pure virtual that access-server implements with the URL-target authority.
The remaining delta items are internal to the pinned modules: persistent epoll in
edge-triggered mode, nginx-style posted-next continuation scheduling, TLS record padding
across nodes, HTTP/1 header timeouts counted from the first byte, idle upstream connections
closed by the peer dropped from the pool, and the QUIC endpoint error-teardown lifecycle. The
latest reviewed delta (from pin `0c56be5fc9e9e106a45b219e49739fedfbaf31b1`) implements the
Efd/RWFd single-loop fd-ownership refactor (`feature/efd_rwfd_refactor.md`): a fd is operated by
exactly one current event loop at any moment, readiness subscriptions fire only on
`Unknown/Blocked → Ready` transitions, RWFd `read`/`write` lambdas always execute and record the
I/O result instead of being readiness-gated, and EventLoop's stop-notification subsystem
(`StopEntry`, `register_stop`/`unregister_stop`) is removed in favor of the close-before-stop
fd-ownership contract — `AcceptFd::close()` defers the waiter wake through a DeferEntry owned by
the awaiter, arming paths refuse a stopping loop with `Canceled`, and the post-stop poller check
asserts no fd remains registered. Loop handover becomes an explicit public protocol:
`TcpStream`/`TlsTcpStream`/`HttpTransport`/`Http1ClientConnection` gain
`detach_for_handover()`/`adopt_loop()` plus `read_ready()`/`write_ready()` and idle
state observation, and the HTTP/1 pool's cross-thread remote return moves from per-entry
`NotifyEntry` posts to a steal-journey state object carried by the returned lease.
`HttpExchange::ResponseChannelClosedAwaiter` is re-derived from the shared `WaitAwaiter` (its
`completed()` query is gone); since `WaitAwaiter::complete()` settles `completed()` at the io
fire instant while the resume is only queued, `when_any` could observe two completed
alternatives at resume time — the delta's follow-up fix `518a7ee` relaxes the winner scan to
the lowest completed index (matching `find_ready`'s arm-time order), which aborts the
WebSocket-over-H2 relay tests otherwise. Access-server adapted in one place:
`ProxyExecutor::execute` now orders the relay task ahead of the channel-closed watch in its
`when_any`, because the lowest-index tie-break must let a finished relay (which closes the
response channel in the same stack) be recorded `Completed` instead of `Canceled`; a lone
channel close still records `Canceled`. The range also moves the poller event batch and its
dispatch cursor into `Poller` (`Poller::wait` takes only a deadline, `dispatch()` consumes the
batch; a Fiber-internal breaking change — access-server does not use the Poller API). No
application source was synchronized back from
upstream as part of this dependency update; the historical import revision above remains
unchanged.

The 2026-09-27 delta replaces BoringSSL's TLS protocol engines with Fiber's in-tree TLS 1.2/1.3
record, handshake, certificate, and ticket engines, including QUIC integration; BoringSSL remains
the crypto provider. It adds P-384 ECDHE, AEAD record bounds checks, per-message handshake limits,
a ten-certificate chain cap, alert interoperability fixes, and TLS fuzz/interop harnesses.
`TlsClientHelloView` moves to `fiber::tls`. Server session tickets now require an explicit
`TlsTicketService`; access-server does not configure one, so downstream connections use full
handshakes without ticket resumption. Existing TLS version and verification policy remain selected
by the product configuration; interoperability must be checked independently of unit tests.

`IoBufChain` resolves its node cache through the current EventLoop instead of storing a pool
pointer. Metrics collection and body piping no longer accept a node pool. All product chain use
and destruction remain on the owning request/collection loop. `HttpTransport` now exposes
append-only `readv` and consuming `writev`; transport test doubles were migrated with bounded
partial reads. `Task::operator co_await` becomes rvalue-only, EventLoopGroup storage is private,
DNS cache upserts take caller time, HTTP/3 attach/header send share a deadline, and route-pattern
errors become returned values. Access-server now checks the matcher insertion result, preserving
its existing invalid-path rejection before snapshot publication.

Dynamic server certificate selection now returns an aliasing shared pointer to the immutable
identity and supplies it to the owning `add_credential` overload that upstream added for issue #41
(`90ed153`); upstream holds that owner in net-layer handshake staging rather than the copied engine
config. The worker hazard still
protects snapshot lookup; the handshake config independently pins just the selected identity after
snapshot reclamation. This adds reference-count operations per handshake, no extra control-block
allocation, PEM parsing, or per-request overhead. Config destruction releases the owner on success,
failure, and cancellation; QUIC releases it at the handshake done-transition. Regression coverage also
stalls a handshake while rotation reclaims the old snapshot, then verifies release after peer close.
The CMake integration supplies Fiber's test support directory to its embedded Nacos and Prometheus
tests. Production script-corpus differential verification and final cutover gates remain unfinished;
this dependency upgrade does not claim production compatibility.

The import preserves upstream source, tests, fixtures, documentation, scripts, and the example
environment file. Repository-integration changes replace upstream-relative test-support includes,
wire the application into `native/CMakeLists.txt`, and update build paths in the copied docs.

The Java compatibility baseline remains `ploto-gateway` revision
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45`. Preserve `LICENSE.upstream` and update this file when
intentionally synchronizing application code from upstream.
