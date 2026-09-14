#include "AccessHttpScriptServices.h"

#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/HttpConnectionGroupKey.h>
#include <fiber/http_script/HttpTarget.h>

#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fiber::access_server {
namespace {

class AccessHttpUpstreamConnection final : public http_script::HttpUpstreamConnection {
public:
    AccessHttpUpstreamConnection(ProxyUpstreamConnection connection, std::string host_header) noexcept :
        connection_(std::move(connection)), host_header_(std::move(host_header)) {}

    [[nodiscard]] http::Http1ClientConnection &connection() noexcept override { return *connection_.connection; }

    [[nodiscard]] std::string_view host_header() const noexcept override { return host_header_; }

private:
    ProxyUpstreamConnection connection_;
    std::string host_header_;
};

std::optional<http::HttpConnectionGroupKey> connection_key(const http_script::HttpTargetSpec &target) noexcept {
    if (target.kind != http_script::HttpTargetSpec::Kind::Url || target.name.empty()) {
        return std::nullopt;
    }
    const std::uint16_t port = target.port != 0 ? target.port : static_cast<std::uint16_t>(target.tls ? 443 : 80);
    const auto scheme =
            target.tls ? http::HttpConnectionGroupKey::Scheme::Https : http::HttpConnectionGroupKey::Scheme::Http;
    // An https URL target must carry a name; an address literal is rejected here
    // (and earlier, at script compile time) because SNI cannot carry a literal.
    return http::HttpConnectionGroupKey::make(target.name, port, scheme);
}

// `host[:port]` for a URL target, mirroring what a client puts in Host: IPv6
// literals are bracketed and the scheme's default port is omitted.
std::string url_target_authority(const http_script::HttpTargetSpec &target) {
    const bool v6_literal = target.name.find(':') != std::string::npos;
    std::string authority;
    authority.reserve(target.name.size() + 8);
    if (v6_literal) {
        authority.push_back('[');
    }
    authority.append(target.name);
    if (v6_literal) {
        authority.push_back(']');
    }
    const std::uint16_t default_port = target.tls ? 443 : 80;
    if (target.port != 0 && target.port != default_port) {
        authority.push_back(':');
        authority.append(std::to_string(target.port));
    }
    return authority;
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
    auto *holder = new (std::nothrow) AccessHttpUpstreamConnection(std::move(*acquired), url_target_authority(target));
    if (!holder) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    co_return std::unique_ptr<http_script::HttpUpstreamConnection>(holder);
}

} // namespace fiber::access_server
