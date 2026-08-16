#include "UpstreamTlsClientPolicy.h"

#include <fiber/net/TlsContext.h>

#include <expected>
#include <utility>

namespace fiber::access_server {

common::IoResult<void> validate_upstream_tls_client_policy(const UpstreamTlsClientPolicy &policy) noexcept {
    if (policy.verification == UpstreamTlsVerificationMode::LegacyInsecure) {
        return policy.ca_file.empty() ? common::IoResult<void>{} : std::unexpected(common::IoErr::Invalid);
    }
    if (policy.verification == UpstreamTlsVerificationMode::SystemCa && !policy.ca_file.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (policy.verification == UpstreamTlsVerificationMode::CustomCa && policy.ca_file.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    net::TlsOptions options;
    options.enabled = true;
    options.verify_peer = true;
    if (policy.verification == UpstreamTlsVerificationMode::CustomCa) {
        options.ca_file = policy.ca_file;
    }
    net::TlsContext context(std::move(options), false, false);
    return context.init();
}

} // namespace fiber::access_server
