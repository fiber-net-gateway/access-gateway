#ifndef FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H
#define FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H

#include <fiber/common/IoError.h>

#include <cstdint>
#include <string>

namespace fiber::access_server {

enum class UpstreamTlsVerificationMode : std::uint8_t {
    LegacyInsecure,
    SystemCa,
    CustomCa,
};

struct UpstreamTlsClientPolicy {
    UpstreamTlsVerificationMode verification = UpstreamTlsVerificationMode::LegacyInsecure;
    std::string ca_file;
};

[[nodiscard]] common::IoResult<void>
validate_upstream_tls_client_policy(const UpstreamTlsClientPolicy &policy) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H
