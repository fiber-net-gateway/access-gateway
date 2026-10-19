#ifndef FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H
#define FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include "../routing/ProxyAddressSelector.h"
#include "AccessRequestHandler.h"
#include "ProxyUpstreamConnection.h"

#include <chrono>
#include <cstddef>

namespace fiber::access_server {

struct ProxyExecutorOptions {
    std::chrono::milliseconds connect_timeout{3000};
    ProxyHappyEyeballsPolicy happy_eyeballs;
    UpstreamTlsClientPolicy upstream_tls;
    std::size_t request_body_chunk_size = 64 * 1024;
    std::size_t response_body_chunk_size = 64 * 1024;
    std::chrono::milliseconds downstream_write_timeout{30000};
    // Floor for body_read_idle_timeout; tests shrink it to keep idle-cut
    // coverage fast.
    std::chrono::milliseconds body_read_idle_floor{60000};
    // Response-body cap applied when the route does not configure
    // max_proxy_body_size; mirrors Java unified-access's
    // fiber.http.client.maxBodySize (600 MiB in the deployed Startup). Zero
    // disables the cap. Tests shrink it to keep coverage fast.
    std::size_t default_max_response_body_size = 600U << 20U;
    // Java cluster-decider fallback policy; absent in tests which pin the
    // pre-decider behavior.
    ProxyClusterFallback cluster_fallback;
};

// Per-read inactivity timeout applied to both body directions: every body read
// must make progress within at least the route timeout, floored at 60s, so a
// silent peer cannot pin a proxied exchange (and its worker slot) forever.
// Deliberate divergence from the Java gateway, whose requestTimer only guards
// the exchange up to response-head arrival. A negative route timeout keeps
// body reads unbounded, mirroring the header-side opt-out.
[[nodiscard]] std::chrono::milliseconds body_read_idle_timeout(std::int32_t timeout_millis,
                                                               std::chrono::milliseconds floor) noexcept;

class ProxyExecutor final : public common::NonCopyable, public common::NonMovable {
public:
    explicit ProxyExecutor(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver = {},
                           ProxyExecutorOptions options = {}) noexcept;

    [[nodiscard]] AccessProxyAdapter adapter() noexcept;
    [[nodiscard]] async::Task<Result<void>> execute(http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                    ProxyExecutionInput input,
                                                    AccessRequestTelemetry &telemetry) noexcept;

private:
    static async::Task<Result<void>> execute_adapter(void *context, http::HttpExchange &exchange,
                                                     const CompiledProxyRoute &proxy, ProxyExecutionInput input,
                                                     AccessRequestTelemetry &telemetry) noexcept;
    [[nodiscard]] async::Task<Result<void>> execute_impl(http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                         ProxyExecutionInput input,
                                                         AccessRequestTelemetry &telemetry) noexcept;

    http::StealableHttp1ConnectionPoolSet &pool_;
    ProxyDnsResolver dns_resolver_{};
    ProxyExecutorOptions options_{};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_EXECUTOR_H
