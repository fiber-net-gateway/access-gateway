# Upstream provenance

The initial `native/access-server` source was imported from
`fiber-net-gateway/fiber-gateway-cpp/apps/access-server` at revision
`0fda7764bf94944aca4b674ab5ab311184703118`.

The application is maintained in this repository after the import. The reusable Fiber runtime,
HTTP, JSON/script, Nacos, CAT, and Prometheus modules are consumed from the pinned
`third_party/fiber-gateway-cpp` submodule. Updating that submodule gitlink is independent of this
historical application import revision.

The current reusable Fiber dependency is pinned at
`a1181674187afa71e55a919ad0fa258c6f7fb706`. The reviewed update range from the previous pin is
`1162be70a08521b14744650bffed8e7c1451ff37..a1181674187afa71e55a919ad0fa258c6f7fb706`.
The complete reviewed range from the original import pin is
`0fda7764bf94944aca4b674ab5ab311184703118..a1181674187afa71e55a919ad0fa258c6f7fb706`.
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
when verifying clients. Access-server was adapted to the reshaped options in the same change; no
wire contract changes were required. No application source was synchronized back from upstream as
part of this dependency update; the historical import revision above remains unchanged.

The import preserves upstream source, tests, fixtures, documentation, scripts, and the example
environment file. Repository-integration changes replace upstream-relative test-support includes,
wire the application into `native/CMakeLists.txt`, and update build paths in the copied docs.

The Java compatibility baseline remains `ploto-gateway` revision
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45`. Preserve `LICENSE.upstream` and update this file when
intentionally synchronizing application code from upstream.
