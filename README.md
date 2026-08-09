# access-gateway
Complete Access Gateway based on the C++23

## Clone

Clone the repository with its pinned Fiber upstream source:

```bash
git clone --recurse-submodules https://github.com/fiber-net-gateway/access-gateway.git
```

For an existing checkout, initialize it with:

```bash
git submodule update --init --recursive
```

## Native access-server

The C++23 data plane is maintained in `native/access-server`. It was initially migrated from the
pinned `third_party/fiber-gateway-cpp/apps/access-server` source; reusable Fiber, HTTP, Nacos, CAT,
and Prometheus code continues to come from that submodule.

Configure, build, and test it from the repository root:

```bash
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_BUILD_TESTS=ON
cmake --build native/build --target fiber_app_access_server --parallel
cmake --build native/build --target fiber_access_server_tests --parallel
ctest --test-dir native/build --output-on-failure -L access-server
```

The executable is written to `native/build/apps/access-server`. Runtime configuration and detailed
compatibility notes are documented in `native/access-server/README.md`.
