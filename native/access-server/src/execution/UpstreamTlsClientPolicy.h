#ifndef FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H
#define FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H

#include "../config/AccessConfig.h"

#include <fiber/common/IoError.h>

#include <memory>
#include <string>
#include <string_view>

namespace fiber::net {
class TlsCredential;
class TrustStore;
} // namespace fiber::net

namespace fiber::access_server {

struct UpstreamTlsClientPolicy {
    UpstreamTlsVerificationMode verification = UpstreamTlsVerificationMode::LegacyInsecure;
    std::string ca_file;
    std::shared_ptr<const net::TrustStore> trust_store;
};

struct UpstreamTlsClientPolicyView {
    UpstreamTlsVerificationMode verification = UpstreamTlsVerificationMode::LegacyInsecure;
    const net::TrustStore *trust_store = nullptr;
    std::string_view server_name;
    std::string_view verify_name;
    const net::TlsCredential *client_credential = nullptr;
};

[[nodiscard]] inline UpstreamTlsClientPolicyView
upstream_tls_client_policy_view(const UpstreamTlsClientPolicy &policy) noexcept {
    return UpstreamTlsClientPolicyView{
            .verification = policy.verification,
            .trust_store = policy.trust_store.get(),
    };
}

[[nodiscard]] common::IoResult<UpstreamTlsClientPolicy>
prepare_upstream_tls_client_policy(UpstreamTlsClientPolicy policy) noexcept;

[[nodiscard]] common::IoResult<void>
validate_upstream_tls_client_policy(const UpstreamTlsClientPolicy &policy) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_POLICY_H
