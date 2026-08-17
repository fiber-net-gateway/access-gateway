#ifndef FIBER_ACCESS_SERVER_PROXY_UPSTREAM_CONNECTION_H
#define FIBER_ACCESS_SERVER_PROXY_UPSTREAM_CONNECTION_H

#include "UpstreamTlsClientPolicy.h"

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/http/StealableHttp1ConnectionPoolSet.h>
#include <fiber/net/HappyEyeballs.h>
#include <fiber/net/IpAddress.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace fiber::http {
class Http1ClientConnection;
}

namespace fiber::access_server {

struct ProxyDnsResolver {
    using Function = async::Task<common::IoResult<std::vector<net::IpAddress>>> (*)(void *context,
                                                                                    std::string_view host) noexcept;

    void *context = nullptr;
    Function resolve = nullptr;
};

struct ProxyHappyEyeballsPolicy {
    bool enabled = true;
    std::chrono::milliseconds connection_attempt_delay{250};
    std::uint8_t max_concurrent_attempts = 2;
    std::uint8_t first_address_family_count = 1;
    net::HappyEyeballsAddressPolicy address_policy = net::HappyEyeballsAddressPolicy::V6First;
};

enum class ProxyConnectErrorCode : std::uint8_t {
    Resolve,
    PoolShutdown,
    Connect,
    Tls,
};

// Per-acquisition facts carried back to the request worker. The fields are
// deliberately fixed and identifier-free so the telemetry layer can update
// pre-bound counters without callbacks or allocation in this coroutine.
struct ProxyConnectionObservation {
    std::uint32_t pool_hits = 0;
    std::uint32_t pool_misses = 0;
    std::uint32_t pool_shutdown = 0;
    std::uint32_t dns_success = 0;
    std::uint32_t dns_empty = 0;
    std::uint32_t dns_failure = 0;
    std::uint32_t dns_unavailable = 0;
    std::uint32_t connect_success = 0;
    std::uint32_t connect_failure = 0;
    std::uint32_t tls_failure = 0;
    std::uint32_t create_failure = 0;
    std::uint16_t connect_candidates = 0;
    std::uint8_t happy_eyeballs_success = 0;
    std::uint8_t happy_eyeballs_failure = 0;
};
static_assert(sizeof(ProxyConnectionObservation) <= 48);

struct ProxyConnectError {
    ProxyConnectErrorCode code = ProxyConnectErrorCode::Connect;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
    ProxyConnectionObservation observation;
};

struct ProxyUpstreamConnection {
    http::StealableHttp1ConnectionPoolSet::Lease lease;
    http::Http1ClientConnection *connection = nullptr;
    ProxyConnectionObservation observation;
};

[[nodiscard]] async::Task<std::expected<ProxyUpstreamConnection, ProxyConnectError>>
acquire_proxy_upstream_connection(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver,
                                  const http::Http1ConnectionGroupKey &key, const UpstreamTlsClientPolicy &tls_policy,
                                  std::chrono::milliseconds connect_timeout,
                                  ProxyHappyEyeballsPolicy happy_eyeballs = {}) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_UPSTREAM_CONNECTION_H
