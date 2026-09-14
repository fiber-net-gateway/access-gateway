#include "ProxyUpstreamConnection.h"
#include "../routing/UpstreamTlsTransportProfile.h"

#include <fiber/common/Assert.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/HttpConnectionGroupKey.h>
#include <fiber/net/SocketAddress.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string>
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
        result.trust_store = profile->trust_store();
    }
    result.server_name = profile->server_name();
    result.verify_name = profile->verify_name();
    result.client_credential = profile->client_credential();
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

// A pinned HTTPS dial (a literal address, or a discovered instance dialed by
// its registered address) carries no name context to authenticate the peer
// with: the handshake sends no SNI and skips peer verification entirely,
// matching the Java gateway. The client certificate (mTLS) still applies.
bool pinned_ip_tls(const http::HttpConnectionGroupKey &key) noexcept {
    return key.scheme() == http::HttpConnectionGroupKey::Scheme::Https && key.has_ip();
}

bool identifiable_tls_failure(const http::HttpConnectionGroupKey &key, UpstreamTlsClientPolicyView policy,
                              common::IoErr io_error) noexcept {
    return key.scheme() == http::HttpConnectionGroupKey::Scheme::Https && verified_tls(policy) && !pinned_ip_tls(key) &&
           (io_error == common::IoErr::Invalid || io_error == common::IoErr::NotSupported);
}

} // namespace

// TLS settings for dialing `key`. Every name the result borrows must outlive the
// connect() call it is passed to: the policy views reference caller-owned
// storage. A named HTTPS host becomes the SNI name unless the policy overrides
// it; a pinned HTTPS dial sends no SNI and does not verify the peer. Exported
// so tests can assert the exact TLS parameters acquisition derives for a
// key/policy pair.
std::optional<http::HttpClientTlsOptions> upstream_connection_tls(const http::HttpConnectionGroupKey &key,
                                                                  UpstreamTlsClientPolicyView tls_policy) {
    if (key.scheme() != http::HttpConnectionGroupKey::Scheme::Https) {
        return std::nullopt;
    }
    if (pinned_ip_tls(key)) {
        http::HttpClientTlsOptions tls;
        tls.security.credential = tls_policy.client_credential;
        return tls;
    }
    http::HttpClientTlsOptions tls;
    tls.security.verify_peer = verified_tls(tls_policy);
    FIBER_ASSERT(!tls.security.verify_peer || tls_policy.trust_store);
    tls.security.trust_store = tls_policy.trust_store;
    if (!tls_policy.server_name.empty()) {
        tls.server_name = tls_policy.server_name;
    } else {
        tls.server_name = key.host();
    }
    if (!tls_policy.verify_name.empty()) {
        tls.verify_name = tls_policy.verify_name;
    }
    tls.security.credential = tls_policy.client_credential;
    return tls;
}

async::Task<std::expected<ProxyUpstreamConnection, ProxyConnectError>>
acquire_proxy_upstream_connection(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver,
                                  const http::HttpConnectionGroupKey &key, UpstreamTlsClientPolicyView tls_policy,
                                  std::chrono::milliseconds connect_timeout,
                                  ProxyHappyEyeballsPolicy happy_eyeballs) noexcept {
    ProxyUpstreamConnection output;
    if (key.scheme() == http::HttpConnectionGroupKey::Scheme::Https && !pinned_ip_tls(key) &&
        verified_tls(tls_policy) && !tls_policy.trust_store) {
        // Fail closed: a verified policy without a materialized trust store is a TLS
        // configuration failure (e.g. an unreadable CA file), never a reason to dial
        // unverified. Report a stable redacted error like a handshake failure.
        ++output.observation.tls_failure;
        co_return std::unexpected(error(ProxyConnectErrorCode::Tls, "upstream TLS trust store is unavailable",
                                        common::IoErr::Invalid, std::move(output.observation)));
    }
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
    if (key.has_ip()) {
        // A literal host or an explicitly pinned dial address: no resolution needed.
        addresses = std::span(&key.ip(), 1);
    } else {
        if (!dns_resolver.resolve) {
            ++output.observation.dns_unavailable;
            co_return std::unexpected(error(ProxyConnectErrorCode::Resolve, "upstream DNS resolver is unavailable",
                                            common::IoErr::NotFound, std::move(output.observation)));
        }
        auto result = co_await dns_resolver.resolve(dns_resolver.context, key.host());
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

    const std::optional<http::HttpClientTlsOptions> tls = upstream_connection_tls(key, tls_policy);

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
        auto emplaced = output.lease.emplace_connection();
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
        auto connected =
                tls ? co_await (*emplaced)->connect(
                              std::span<const net::SocketAddress>(candidates.data(), addresses.size()), options, *tls)
                    : co_await (*emplaced)->connect(
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

        auto emplaced = output.lease.emplace_connection();
        if (!emplaced) {
            ++output.observation.create_failure;
            last_error = emplaced.error();
            output.lease.reset();
            continue;
        }
        const net::SocketAddress peer(addresses[i], key.port());
        auto connected = tls ? co_await (*emplaced)->connect(peer, connect_timeout, *tls)
                             : co_await (*emplaced)->connect(peer, connect_timeout);
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
