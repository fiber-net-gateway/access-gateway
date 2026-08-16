#ifndef FIBER_ACCESS_SERVER_CLIENT_METADATA_H
#define FIBER_ACCESS_SERVER_CLIENT_METADATA_H

#include "../routing/Cidr.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::http {
class HttpExchange;
class HttpHeaders;
} // namespace fiber::http

namespace fiber::access_server {

inline constexpr std::size_t kMaxTrustedProxyCidrs = 64;
inline constexpr std::size_t kMaxForwardedHops = 32;

enum class ClientMetadataMode : std::uint8_t {
    Direct,
    TrustedProxy,
    LegacyHeaders,
};

enum class ClientAddressSource : std::uint8_t {
    SocketPeer,
    Forwarded,
    XForwardedFor,
    XRealIp,
    LegacyXRealIp,
};

enum class ClientSchemeSource : std::uint8_t {
    Listener,
    Forwarded,
    XForwardedProto,
    LegacyXForwardedProto,
};

enum class ForwardingStatus : std::uint8_t {
    NotPresent,
    Trusted,
    IgnoredUntrustedPeer,
    IgnoredDirectMode,
    Invalid,
    Legacy,
};

struct ClientMetadataResolverOptions {
    ClientMetadataMode mode = ClientMetadataMode::Direct;
    std::vector<Cidr> trusted_proxy_cidrs;
    bool connection_secure = false;
};

struct ClientMetadata {
    net::IpAddress peer_address{};
    net::IpAddress client_address{};
    std::optional<Cidr> route_policy_target;
    std::optional<Cidr> gray_target;
    std::string_view external_scheme = "http";
    ClientAddressSource address_source = ClientAddressSource::SocketPeer;
    ClientSchemeSource scheme_source = ClientSchemeSource::Listener;
    ForwardingStatus forwarding_status = ForwardingStatus::NotPresent;
    bool has_client_address = true;
    bool secure = false;
    bool peer_trusted = false;
};

class ClientMetadataResolver {
public:
    explicit ClientMetadataResolver(ClientMetadataResolverOptions options = {}) noexcept;

    [[nodiscard]] ClientMetadata resolve(const http::HttpExchange &exchange) const noexcept;
    [[nodiscard]] ClientMetadata resolve(const net::SocketAddress &peer,
                                         const http::HttpHeaders &headers) const noexcept;
    [[nodiscard]] const ClientMetadataResolverOptions &options() const noexcept { return options_; }

private:
    [[nodiscard]] bool is_trusted_proxy(const net::IpAddress &address) const noexcept;

    ClientMetadataResolverOptions options_;
};

[[nodiscard]] std::string_view client_metadata_mode_name(ClientMetadataMode mode) noexcept;
[[nodiscard]] std::string_view client_address_source_name(ClientAddressSource source) noexcept;
[[nodiscard]] std::string_view client_scheme_source_name(ClientSchemeSource source) noexcept;
[[nodiscard]] std::string_view forwarding_status_name(ForwardingStatus status) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_CLIENT_METADATA_H
