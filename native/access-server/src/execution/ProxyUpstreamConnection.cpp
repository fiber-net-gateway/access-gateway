#include "ProxyUpstreamConnection.h"
#include "../routing/UpstreamTlsTransportProfile.h"

#include <fiber/common/Assert.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/net/SocketAddress.h>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <utility>

namespace fiber::access_server {

UpstreamTlsClientPolicyView effective_upstream_tls_client_policy(const UpstreamTlsClientPolicy &environment,
                                                                 const UpstreamTlsTransportProfile *profile) noexcept {
    UpstreamTlsClientPolicyView result = upstream_tls_client_policy_view(environment);
    if (!profile) {
        return result;
    }
    if (profile->verification() != UpstreamTlsVerificationMode::Inherit) {
        result.verification = profile->verification();
        result.ca_file = profile->verification() == UpstreamTlsVerificationMode::CustomCa ? profile->ca_file()
                                                                                          : std::string_view{};
    }
    result.server_name = profile->server_name();
    result.verify_name = profile->verify_name();
    result.client_certificate_file = profile->client_certificate_file();
    result.client_private_key_file = profile->client_private_key_file();
    return result;
}

namespace {

ProxyConnectError error(ProxyConnectErrorCode code, const char *message, common::IoErr io_error,
                        ProxyConnectionObservation observation) noexcept {
    return ProxyConnectError{
            .code = code,
            .io_error = io_error,
            .message = message,
            .observation = std::move(observation),
    };
}

bool verified_tls(UpstreamTlsClientPolicyView policy) noexcept {
    return policy.verification != UpstreamTlsVerificationMode::LegacyInsecure;
}

bool identifiable_tls_failure(const http::Http1ConnectionGroupKey &key, UpstreamTlsClientPolicyView policy,
                              common::IoErr io_error) noexcept {
    return key.scheme() == http::Http1ConnectionGroupKey::Scheme::Https && verified_tls(policy) &&
           (io_error == common::IoErr::Invalid || io_error == common::IoErr::NotSupported);
}

http::Http1ClientConnectionOptions connection_options(const http::Http1ConnectionGroupKey &key,
                                                      const net::IpAddress &ip,
                                                      UpstreamTlsClientPolicyView tls_policy) {
    http::Http1ClientConnectionOptions result;
    result.peer_addr = net::SocketAddress(ip, key.port());
    result.pool_affinity = key.pool_affinity();
    if (key.scheme() == http::Http1ConnectionGroupKey::Scheme::Https) {
        result.tls.enabled = true;
        result.tls.verify_peer = verified_tls(tls_policy);
        if (tls_policy.verification == UpstreamTlsVerificationMode::CustomCa) {
            FIBER_ASSERT(!tls_policy.ca_file.empty());
            result.tls.ca_file.assign(tls_policy.ca_file);
        }
        if (!tls_policy.server_name.empty()) {
            result.tls.server_name.assign(tls_policy.server_name);
        } else if (key.is_name()) {
            result.tls.server_name.assign(key.host_name());
        }
        if (!tls_policy.verify_name.empty()) {
            result.tls.verify_name.assign(tls_policy.verify_name);
        } else if (key.is_ip() && result.tls.verify_peer && tls_policy.server_name.empty()) {
            // IP literals are authenticated as IP identities without emitting an
            // IP-valued SNI extension.
            result.tls.verify_name = key.ip_address().to_string();
        }
        if (!tls_policy.client_certificate_file.empty()) {
            FIBER_ASSERT(!tls_policy.client_private_key_file.empty());
            result.tls.cert_file.assign(tls_policy.client_certificate_file);
            result.tls.key_file.assign(tls_policy.client_private_key_file);
        }
    }
    return result;
}

} // namespace

async::Task<std::expected<ProxyUpstreamConnection, ProxyConnectError>>
acquire_proxy_upstream_connection(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver,
                                  const http::Http1ConnectionGroupKey &key, UpstreamTlsClientPolicyView tls_policy,
                                  std::chrono::milliseconds connect_timeout,
                                  ProxyHappyEyeballsPolicy happy_eyeballs) noexcept {
    ProxyUpstreamConnection output;
    output.lease = co_await pool.acquire(key);
    if (!output.lease.valid()) {
        ++output.observation.pool_shutdown;
        co_return std::unexpected(error(ProxyConnectErrorCode::PoolShutdown,
                                        "upstream connection pool is shutting down", common::IoErr::Canceled,
                                        std::move(output.observation)));
    }
    if (output.lease.hit()) {
        ++output.observation.pool_hits;
    } else {
        ++output.observation.pool_misses;
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
            ++output.observation.dns_unavailable;
            co_return std::unexpected(error(ProxyConnectErrorCode::Resolve, "upstream DNS resolver is unavailable",
                                            common::IoErr::NotFound, std::move(output.observation)));
        }
        auto result = co_await dns_resolver.resolve(dns_resolver.context, key.host_name());
        if (!result) {
            ++output.observation.dns_failure;
            co_return std::unexpected(error(ProxyConnectErrorCode::Resolve, "upstream DNS resolution failed",
                                            result.error(), std::move(output.observation)));
        }
        resolved = std::move(*result);
        addresses = resolved;
        if (addresses.empty()) {
            ++output.observation.dns_empty;
        } else {
            ++output.observation.dns_success;
        }
    }
    if (addresses.empty()) {
        co_return std::unexpected(error(ProxyConnectErrorCode::Resolve, "upstream DNS returned no address",
                                        common::IoErr::NotFound, std::move(output.observation)));
    }

    output.observation.connect_candidates = static_cast<std::uint16_t>(
            std::min<std::size_t>(addresses.size(), std::numeric_limits<std::uint16_t>::max()));

    if (happy_eyeballs.enabled && addresses.size() > 1) {
        if (addresses.size() > net::kHappyEyeballsMaxAddresses) {
            ++output.observation.connect_failure;
            ++output.observation.happy_eyeballs_failure;
            co_return std::unexpected(error(ProxyConnectErrorCode::Connect,
                                            "upstream DNS returned too many connection candidates",
                                            common::IoErr::Invalid, std::move(output.observation)));
        }

        std::array<net::SocketAddress, net::kHappyEyeballsMaxAddresses> candidates;
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            candidates[i] = net::SocketAddress(addresses[i], key.port());
        }
        auto emplaced = output.lease.emplace_connection(connection_options(key, addresses.front(), tls_policy));
        if (!emplaced) {
            ++output.observation.create_failure;
            co_return std::unexpected(error(ProxyConnectErrorCode::Connect, "failed to create upstream connection",
                                            emplaced.error(), std::move(output.observation)));
        }
        net::HappyEyeballsOptions options{
                .total_timeout = connect_timeout,
                .connection_attempt_delay = happy_eyeballs.connection_attempt_delay,
                .max_concurrent_attempts = happy_eyeballs.max_concurrent_attempts,
                .first_address_family_count = happy_eyeballs.first_address_family_count,
                .address_policy = happy_eyeballs.address_policy,
        };
        auto connected = co_await (*emplaced)->connect(
                std::span<const net::SocketAddress>(candidates.data(), addresses.size()), options);
        if (connected) {
            ++output.observation.connect_success;
            ++output.observation.happy_eyeballs_success;
            output.connection = *emplaced;
            co_return std::move(output);
        }
        const common::IoErr connect_error = connected.error();
        if (identifiable_tls_failure(key, tls_policy, connect_error)) {
            ++output.observation.tls_failure;
        } else {
            ++output.observation.connect_failure;
        }
        ++output.observation.happy_eyeballs_failure;
        output.lease.reset();
        const bool tls_failure = identifiable_tls_failure(key, tls_policy, connect_error);
        co_return std::unexpected(error(tls_failure ? ProxyConnectErrorCode::Tls : ProxyConnectErrorCode::Connect,
                                        tls_failure ? "upstream TLS negotiation failed" : "upstream connection failed",
                                        connect_error, std::move(output.observation)));
    }

    common::IoErr last_error = common::IoErr::NotFound;
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0) {
            output.lease = co_await pool.acquire(key);
            if (!output.lease.valid()) {
                ++output.observation.pool_shutdown;
                co_return std::unexpected(error(ProxyConnectErrorCode::PoolShutdown,
                                                "upstream connection pool is shutting down", common::IoErr::Canceled,
                                                std::move(output.observation)));
            }
            if (output.lease.hit()) {
                ++output.observation.pool_hits;
            } else {
                ++output.observation.pool_misses;
            }
            if (output.lease.has_connection()) {
                output.connection = output.lease.get();
                co_return std::move(output);
            }
        }

        auto emplaced = output.lease.emplace_connection(connection_options(key, addresses[i], tls_policy));
        if (!emplaced) {
            ++output.observation.create_failure;
            last_error = emplaced.error();
            output.lease.reset();
            continue;
        }
        auto connected = co_await (*emplaced)->connect(connect_timeout);
        if (connected) {
            ++output.observation.connect_success;
            output.connection = *emplaced;
            co_return std::move(output);
        }
        last_error = connected.error();
        if (identifiable_tls_failure(key, tls_policy, last_error)) {
            ++output.observation.tls_failure;
        } else {
            ++output.observation.connect_failure;
        }
        output.lease.reset();
    }
    const bool tls_failure = identifiable_tls_failure(key, tls_policy, last_error);
    co_return std::unexpected(error(tls_failure ? ProxyConnectErrorCode::Tls : ProxyConnectErrorCode::Connect,
                                    tls_failure ? "upstream TLS negotiation failed" : "upstream connection failed",
                                    last_error, std::move(output.observation)));
}

} // namespace fiber::access_server
