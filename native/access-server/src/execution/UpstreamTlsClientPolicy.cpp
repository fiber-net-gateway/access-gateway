#include "UpstreamTlsClientPolicy.h"

#include <fiber/net/TrustStore.h>

#include <expected>
#include <utility>

namespace fiber::access_server {

common::IoResult<UpstreamTlsClientPolicy> prepare_upstream_tls_client_policy(UpstreamTlsClientPolicy policy) noexcept {
    if (policy.verification == UpstreamTlsVerificationMode::Inherit) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (policy.verification == UpstreamTlsVerificationMode::LegacyInsecure) {
        if (!policy.ca_file.empty()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        policy.trust_store.reset();
        return policy;
    }
    if (policy.verification == UpstreamTlsVerificationMode::SystemCa && !policy.ca_file.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (policy.verification == UpstreamTlsVerificationMode::CustomCa && policy.ca_file.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const net::TrustStoreOptions options = policy.verification == UpstreamTlsVerificationMode::CustomCa
                                                   ? net::TrustStoreOptions::from_file(policy.ca_file)
                                                   : net::TrustStoreOptions::system();
    auto trust_store = net::TrustStore::create(options);
    if (!trust_store) {
        return std::unexpected(trust_store.error());
    }
    policy.trust_store = std::shared_ptr<const net::TrustStore>(std::move(*trust_store));
    return policy;
}

common::IoResult<void> validate_upstream_tls_client_policy(const UpstreamTlsClientPolicy &policy) noexcept {
    auto prepared = prepare_upstream_tls_client_policy(policy);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    return {};
}

} // namespace fiber::access_server
