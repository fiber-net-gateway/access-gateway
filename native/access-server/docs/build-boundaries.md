# Native component and build boundaries

The access-server native code is compiled as five explicit static components instead of one
catch-all archive. The split makes the offline validator independent of online runtime clients and
turns source ownership into a link-time constraint.

## Target graph

```text
fiber_app_access_gateway_validator
  -> access_server_validation
       -> access_server_config
            -> fiber_lib + zlib

fiber_app_access_server
  -> access_server_runtime
       -> access_server_execution
            -> access_server_observability
                 -> access_server_config
       -> fiber::nacos + fiber::cat + fiber::prometheus
```

The concrete targets also have `access_server::<component>` aliases. Source lists remain explicit;
no target uses a source glob.

| Target | Owned responsibility | Direct reusable dependencies |
| --- | --- | --- |
| `access_server_config` | wire codecs, limits, immutable routing model, Host/CIDR, route and script compilation, gray compilation, deterministic static gzip | `fiber_lib`, zlib |
| `access_server_observability` | bounded metrics, activation evidence, access-log policy, process stats, trace state | config, Fiber CAT/Prometheus |
| `access_server_execution` | request handler, response/proxy pipeline, request-coupled telemetry and trace facade | config, observability, Fiber CAT |
| `access_server_runtime` | Nacos/discovery, watchers, DNS, listeners, workers, publication and ordered lifecycle | config, execution, observability, Fiber Nacos/CAT/Prometheus |
| `access_server_validation` | versioned offline validator protocol and result encoding | config only |

Request-coupled observability implementations live in the execution archive because their public
operations and `AccessResult`/connection observations form one request pipeline. Aggregate metrics
and evidence remain in the observability archive and do not depend on execution or runtime.

## Dependency-breaking adapters

Two compilation concerns are deliberately below runtime:

- `AccessScriptCompiler` owns the long-lived script libraries and extension userdata used by
  immutable compiled routes. `AccessScriptRuntime` is a stateless request execution adapter.
- `GrayMatchCompiler` builds the Java-compatible bounded CIDR/ratio model. `GrayMatchStore` adds
  generations, worker-local publication and sampling, while the validator reuses the same pure
  compiler after its stricter publication checks.

`AccessRequestHandler` consumes an `AccessRouteSnapshotProvider`, a two-pointer value adapter. It no
longer includes or links `RouteConfigStore`; the runtime supplies the provider. This keeps execution
independent of Nacos while preserving one acquire-load snapshot pin per request and the existing
old-snapshot lifetime guarantee.

## Validator-only build gate

`ACCESS_SERVER_BUILD_RUNTIME` defaults to `ON`. Setting it to `OFF` also disables Fiber Nacos, CAT,
and Prometheus targets, but keeps config, validation and the validator executable available:

```bash
cmake -S native -B native/build-validator-only -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_BUILD_TESTS=OFF \
  -DACCESS_SERVER_BUILD_RUNTIME=OFF
cmake --build native/build-validator-only \
  --target fiber_app_access_gateway_validator --parallel
```

The final validator link must contain `access_server_validation`, `access_server_config`,
`fiber_lib`, TLS/crypto and zlib. It must not contain Nacos, protobuf, CAT, Prometheus or any
access-server runtime/execution archive. This clean build is the authoritative boundary check; a
successful full-runtime build alone is insufficient because link-time dead stripping could hide an
accidental dependency.

## Change rules

- A config or validation public header must not include `src/runtime/`, `src/execution/`, Nacos,
  CAT or Prometheus headers.
- Runtime transport and lifecycle types must not be added to a compiler callback. Cross-boundary
  calls use small typed adapters or immutable values.
- Build revision macros belong only to `access_server_runtime`; the validator output is a pure
  function of its request and the compiled contract.
- New sources are registered in exactly one component. A dependency is `PUBLIC` only when its
  types occur in a component's public headers; implementation-only dependencies stay `PRIVATE`.
- Keep compatible IPO/LTO settings on every component so the split does not create an artificial
  optimization barrier in final executables.
