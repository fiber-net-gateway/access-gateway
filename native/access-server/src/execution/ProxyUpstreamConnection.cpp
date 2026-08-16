#include "ProxyUpstreamConnection.h"

#include <fiber/common/Assert.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/net/SocketAddress.h>

#include <span>
#include <utility>

namespace fiber::access_server {
namespace {

ProxyConnectError error(ProxyConnectErrorCode code, const char *message,
                        common::IoErr io_error = common::IoErr::None) noexcept {
    return ProxyConnectError{
            .code = code,
            .io_error = io_error,
            .message = message,
    };
}

bool verified_tls(const UpstreamTlsClientPolicy &policy) noexcept {
    return policy.verification != UpstreamTlsVerificationMode::LegacyInsecure;
}

bool identifiable_tls_failure(const http::Http1ConnectionGroupKey &key, const UpstreamTlsClientPolicy &policy,
                              common::IoErr io_error) noexcept {
    return key.scheme() == http::Http1ConnectionGroupKey::Scheme::Https && verified_tls(policy) &&
           (io_error == common::IoErr::Invalid || io_error == common::IoErr::NotSupported);
}

http::Http1ClientConnectionOptions connection_options(const http::Http1ConnectionGroupKey &key,
                                                      const net::IpAddress &ip,
                                                      const UpstreamTlsClientPolicy &tls_policy) {
    http::Http1ClientConnectionOptions result;
    result.peer_addr = net::SocketAddress(ip, key.port());
    if (key.scheme() == http::Http1ConnectionGroupKey::Scheme::Https) {
        result.tls.enabled = true;
        result.tls.verify_peer = verified_tls(tls_policy);
        if (tls_policy.verification == UpstreamTlsVerificationMode::CustomCa) {
            FIBER_ASSERT(!tls_policy.ca_file.empty());
            result.tls.ca_file = tls_policy.ca_file;
        }
        if (key.is_name()) {
            result.tls.server_name.assign(key.host_name());
        } else if (result.tls.verify_peer) {
            // IP literals are authenticated as IP identities without emitting an
            // IP-valued SNI extension.
            result.tls.verify_name = key.ip_address().to_string();
        }
    }
    return result;
}

} // namespace

async::Task<std::expected<ProxyUpstreamConnection, ProxyConnectError>>
acquire_proxy_upstream_connection(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver,
                                  const http::Http1ConnectionGroupKey &key, const UpstreamTlsClientPolicy &tls_policy,
                                  std::chrono::milliseconds connect_timeout) noexcept {
    ProxyUpstreamConnection output;
    output.lease = co_await pool.acquire(key);
    if (!output.lease.valid()) {
        co_return std::unexpected(error(ProxyConnectErrorCode::PoolShutdown,
                                        "upstream connection pool is shutting down", common::IoErr::Canceled));
    }
    if (output.lease.has_connection()) {
        output.connection = output.lease.get();
        co_return std::move(output);
    }

    std::vector<net::IpAddress> resolved;
    std::span<const net::IpAddress> addresses;
    if (key.is_ip()) {
        addresses = std::span(&key.ip_address(), 1);
    } else {
        if (!dns_resolver.resolve) {
            co_return std::unexpected(error(ProxyConnectErrorCode::Resolve, "upstream DNS resolver is unavailable",
                                            common::IoErr::NotFound));
        }
        auto result = co_await dns_resolver.resolve(dns_resolver.context, key.host_name());
        if (!result) {
            co_return std::unexpected(
                    error(ProxyConnectErrorCode::Resolve, "upstream DNS resolution failed", result.error()));
        }
        resolved = std::move(*result);
        addresses = resolved;
    }
    if (addresses.empty()) {
        co_return std::unexpected(
                error(ProxyConnectErrorCode::Resolve, "upstream DNS returned no address", common::IoErr::NotFound));
    }

    common::IoErr last_error = common::IoErr::NotFound;
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0) {
            output.lease = co_await pool.acquire(key);
            if (!output.lease.valid()) {
                co_return std::unexpected(error(ProxyConnectErrorCode::PoolShutdown,
                                                "upstream connection pool is shutting down", common::IoErr::Canceled));
            }
            if (output.lease.has_connection()) {
                output.connection = output.lease.get();
                co_return std::move(output);
            }
        }

        auto emplaced = output.lease.emplace_connection(connection_options(key, addresses[i], tls_policy));
        if (!emplaced) {
            last_error = emplaced.error();
            output.lease.reset();
            continue;
        }
        auto connected = co_await (*emplaced)->connect(connect_timeout);
        if (connected) {
            output.connection = *emplaced;
            co_return std::move(output);
        }
        last_error = connected.error();
        output.lease.reset();
    }
    const bool tls_failure = identifiable_tls_failure(key, tls_policy, last_error);
    co_return std::unexpected(error(tls_failure ? ProxyConnectErrorCode::Tls : ProxyConnectErrorCode::Connect,
                                    tls_failure ? "upstream TLS negotiation failed" : "upstream connection failed",
                                    last_error));
}

} // namespace fiber::access_server
