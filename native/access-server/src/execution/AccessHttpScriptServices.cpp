#include "AccessHttpScriptServices.h"

#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/http_script/HttpTarget.h>
#include <fiber/net/IpAddress.h>

#include <cstdint>
#include <new>
#include <optional>
#include <utility>

namespace fiber::access_server {
namespace {

class AccessHttpUpstreamConnection final : public http_script::HttpUpstreamConnection {
public:
    explicit AccessHttpUpstreamConnection(ProxyUpstreamConnection connection) noexcept :
        connection_(std::move(connection)) {}

    [[nodiscard]] http::Http1ClientConnection &connection() noexcept override { return *connection_.connection; }

private:
    ProxyUpstreamConnection connection_;
};

std::optional<http::Http1ConnectionGroupKey> connection_key(const http_script::HttpTargetSpec &target) noexcept {
    if (target.kind != http_script::HttpTargetSpec::Kind::Url || target.name.empty()) {
        return std::nullopt;
    }
    const std::uint16_t port = target.port != 0 ? target.port : static_cast<std::uint16_t>(target.tls ? 443 : 80);
    const auto scheme =
            target.tls ? http::Http1ConnectionGroupKey::Scheme::Https : http::Http1ConnectionGroupKey::Scheme::Http;
    net::IpAddress ip;
    if (net::IpAddress::parse(target.name, ip)) {
        return http::Http1ConnectionGroupKey::from_ip(ip, port, scheme);
    }
    return http::Http1ConnectionGroupKey::from_name(target.name, port, scheme);
}

} // namespace

AccessHttpScriptServices::AccessHttpScriptServices(http::StealableHttp1ConnectionPoolSet &pool,
                                                   ProxyDnsResolver dns_resolver, UpstreamTlsClientPolicy tls_policy,
                                                   ProxyHappyEyeballsPolicy happy_eyeballs) noexcept :
    pool_(&pool), dns_resolver_(dns_resolver), tls_policy_(std::move(tls_policy)), happy_eyeballs_(happy_eyeballs) {}

async::Task<common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
AccessHttpScriptServices::acquire(const http_script::HttpTargetSpec &target,
                                  std::chrono::milliseconds connect_timeout) noexcept {
    auto key = connection_key(target);
    if (!key) {
        co_return std::unexpected(target.kind == http_script::HttpTargetSpec::Kind::Upstream
                                          ? common::IoErr::NotSupported
                                          : common::IoErr::Invalid);
    }
    auto acquired = co_await acquire_proxy_upstream_connection(*pool_, dns_resolver_, *key, tls_policy_,
                                                               connect_timeout, happy_eyeballs_);
    if (!acquired) {
        co_return std::unexpected(acquired.error().io_error);
    }
    auto *holder = new (std::nothrow) AccessHttpUpstreamConnection(std::move(*acquired));
    if (!holder) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    co_return std::unique_ptr<http_script::HttpUpstreamConnection>(holder);
}

} // namespace fiber::access_server
