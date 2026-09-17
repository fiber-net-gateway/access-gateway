# Upstream provenance

The initial `native/access-server` source was imported from
`fiber-net-gateway/fiber-gateway-cpp/apps/access-server` at revision
`0fda7764bf94944aca4b674ab5ab311184703118`.

The application is maintained in this repository after the import. The reusable Fiber runtime,
HTTP, JSON/script, Nacos, CAT, and Prometheus modules are consumed from the pinned
`third_party/fiber-gateway-cpp` submodule. Updating that submodule gitlink is independent of this
historical application import revision.

The current reusable Fiber dependency is pinned at
`054b36878671ea77ec84ec2fd1fc790cb9b09623`. The pinned revision carries no repository-owned
compatibility patches: the four defects that previously required patches under `native/patches/`
are fixed inside this reviewed range (see `native/patches/README.md` for the retired-patch map).
The repository-owned regression
`ProxyExecutorTest.StreamsChunkedUpstreamWhoseFramingArrivesSeparatelyFromPayload` still guards
the HTTP/1 chunked split-framing fix from the application side.
The reviewed update range from the previous pin is
`0c56be5fc9e9e106a45b219e49739fedfbaf31b1..054b36878671ea77ec84ec2fd1fc790cb9b09623`.
The complete reviewed range from the original import pin is
`0fda7764bf94944aca4b674ab5ab311184703118..054b36878671ea77ec84ec2fd1fc790cb9b09623`.
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

The import preserves upstream source, tests, fixtures, documentation, scripts, and the example
environment file. Repository-integration changes replace upstream-relative test-support includes,
wire the application into `native/CMakeLists.txt`, and update build paths in the copied docs.

The Java compatibility baseline remains `ploto-gateway` revision
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45`. Preserve `LICENSE.upstream` and update this file when
intentionally synchronizing application code from upstream.
