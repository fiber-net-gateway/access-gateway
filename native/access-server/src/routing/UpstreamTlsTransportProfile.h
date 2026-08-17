#ifndef FIBER_ACCESS_SERVER_UPSTREAM_TLS_TRANSPORT_PROFILE_H
#define FIBER_ACCESS_SERVER_UPSTREAM_TLS_TRANSPORT_PROFILE_H

#include "../config/AccessConfig.h"
#include "../config/AccessConfigError.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/http/Http1ConnectionGroupKey.h>

namespace fiber::access_server {

class UpstreamTlsCaBundle;

// Connection-bound route TLS settings. The sealed CA descriptor is owned by
// the same immutable route snapshot that owns this profile, so a request which
// pins an old snapshot can still finish while a newer generation is published.
class UpstreamTlsTransportProfile final {
public:
    UpstreamTlsTransportProfile(UpstreamTlsTransportProfile &&other) noexcept;
    UpstreamTlsTransportProfile &operator=(UpstreamTlsTransportProfile &&other) noexcept;
    ~UpstreamTlsTransportProfile();

    UpstreamTlsTransportProfile(const UpstreamTlsTransportProfile &) = default;
    UpstreamTlsTransportProfile &operator=(const UpstreamTlsTransportProfile &) = default;

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t pool_affinity() const noexcept { return pool_affinity_; }
    [[nodiscard]] UpstreamTlsVerificationMode verification() const noexcept { return verification_; }
    [[nodiscard]] std::string_view ca_file() const noexcept;
    [[nodiscard]] std::string_view server_name() const noexcept { return server_name_; }
    [[nodiscard]] std::string_view verify_name() const noexcept { return verify_name_; }
    [[nodiscard]] std::optional<http::Http1ConnectionGroupKey>
    connection_key(const http::Http1ConnectionGroupKey &base) const noexcept;

private:
    friend std::expected<UpstreamTlsTransportProfile, AccessConfigError>
    compile_upstream_tls_transport_profile(const RouteUpstreamTlsConfig &config, std::size_t route_index);

    UpstreamTlsTransportProfile() = default;

    std::shared_ptr<const UpstreamTlsCaBundle> ca_bundle_;
    std::string server_name_;
    std::string verify_name_;
    std::uint64_t generation_ = 0;
    std::uint64_t pool_affinity_ = 0;
    UpstreamTlsVerificationMode verification_ = UpstreamTlsVerificationMode::Inherit;
};

[[nodiscard]] std::expected<UpstreamTlsTransportProfile, AccessConfigError>
compile_upstream_tls_transport_profile(const RouteUpstreamTlsConfig &config, std::size_t route_index);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_UPSTREAM_TLS_TRANSPORT_PROFILE_H
