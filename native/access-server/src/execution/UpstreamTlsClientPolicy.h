#ifndef FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H
#define FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H

#include "../config/AccessConfig.h"

#include <fiber/common/IoError.h>

#include <string>
#include <string_view>

namespace fiber::access_server {

struct UpstreamTlsClientPolicy {
    UpstreamTlsVerificationMode verification = UpstreamTlsVerificationMode::LegacyInsecure;
    std::string ca_file;
};

struct UpstreamTlsClientPolicyView {
    UpstreamTlsVerificationMode verification = UpstreamTlsVerificationMode::LegacyInsecure;
    std::string_view ca_file;
    std::string_view server_name;
    std::string_view verify_name;
};

[[nodiscard]] inline UpstreamTlsClientPolicyView
upstream_tls_client_policy_view(const UpstreamTlsClientPolicy &policy) noexcept {
    return UpstreamTlsClientPolicyView{
            .verification = policy.verification,
            .ca_file = policy.ca_file,
    };
}

[[nodiscard]] common::IoResult<void>
validate_upstream_tls_client_policy(const UpstreamTlsClientPolicy &policy) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H
