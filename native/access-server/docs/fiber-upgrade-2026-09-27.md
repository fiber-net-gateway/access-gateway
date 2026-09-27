# Fiber dependency upgrade — 2026-09-27

The reviewed dependency range is
`054b36878671ea77ec84ec2fd1fc790cb9b09623..270035b7707b8d3825e31eca2d1c67d1da1ef640`.
The latter was verified as upstream `master`. The pre-existing working checkout at `040ff9af`
was clean and is an ancestor of the new pin. The historical application import and Java
compatibility baseline remain unchanged.

## Integration changes

- Migrate ClientHello views to `fiber::tls`, metrics/body-pipe calls to implicit EventLoop node
  pools, and HTTP transport test doubles to bounded append-only reads and consuming writes.
- Check returned route-pattern errors before publishing a snapshot. Existing product path
  validation and rnacos wire contracts remain unchanged.
- Pin dynamically selected TLS identities independently of the published certificate snapshot.
  The owning `add_credential` overload comes from upstream `90ed153`, which closed
  [issue #41](https://github.com/fiber-net-gateway/fiber-gateway-cpp/issues/41); the temporary
  compatibility patch carried against `f832d545` was dropped. Aliasing shared pointers add
  reference counting at handshake setup without allocating a control block or changing the
  ordinary request path.
- Supply the embedded Nacos and Prometheus tests with Fiber's test-support include directory.
- Adapt the TLS identity benchmark and remove stale `delete` expressions for stack-owned
  loopback benchmark fixtures; their server objects already stop and release on the owner loop.

The upstream TLS protocol implementation now uses its own TLS 1.2/1.3 engines over BoringSSL
crypto. Access-server does not configure the new ticket service, so server ticket resumption is
not enabled. This operational change and the full dependency review are recorded in
[UPSTREAM.md](../UPSTREAM.md).

## Validation

Release configuration and the complete build, including the runtime, offline validator, Fiber
component tests, access-server tests, and optional product benchmarks:

```sh
cmake -S native -B native/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFIBER_BUILD_TESTS=ON \
  -DACCESS_SERVER_BUILD_BENCHMARKS=ON
cmake --build native/build --parallel 12
ctest --test-dir native/build --output-on-failure --parallel 6 --timeout 120
```

CTest registered 2753 tests: 2748 passed and five skipped. The `access-server` label accounted for
380 tests (379 passed, one skipped). Skips require explicitly enabled external services or input:

- `Http3ClientTest.NginxInterop`
- `NacosConfigServiceTest.RnacosInteropWhenEnabled`
- `NacosNamingServiceTest.RnacosInteropWhenEnabled`
- `NacosRpcTest.RnacosInteropWhenEnabled`
- `ProductionScriptCorpusTest.CompilesExternalSnapshotWhenProvided`

Small deterministic benchmark smoke runs also passed; these are functional checks, not performance
comparison measurements:

```sh
native/build/access-server/fiber_access_tls_identity_benchmark 10 2
native/build/access-server/fiber_access_proxy_loopback_benchmark 10 2
```

The focused certificate tests cover snapshot reclamation while a handshake remains suspended,
release on peer close, and successful handshakes across rotation. Address/undefined-behavior
sanitizer validation uses:

```sh
cmake -S native -B native/build-address -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFIBER_BUILD_TESTS=ON \
  -DFIBER_ENABLE_LTO=OFF -DACCESS_SERVER_SANITIZER=address
cmake --build native/build-address --target fiber_access_server_tests --parallel 10
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir native/build-address --output-on-failure \
  -R '^(TlsCertificateStoreTest|TlsCertificateConfigTest|ProxyExecutorTest|AccessServerTest)\.' \
  --parallel 2 --timeout 120
```

Changed C++ files were checked with clang-format 22 against the pinned
Fiber style. `bash -n native/patches/apply.sh` and `git diff --check` passed. The submodule working
tree remained clean. Production script-corpus differential verification and final cutover gates
remain unfinished; this upgrade does not establish production compatibility.
