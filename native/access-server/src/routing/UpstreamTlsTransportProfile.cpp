#include "UpstreamTlsTransportProfile.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>

#include <openssl/sha.h>

#include <fiber/net/IpAddress.h>
#include <fiber/net/TrustStore.h>

namespace fiber::access_server {
namespace {

AccessConfigError profile_error(AccessConfigErrorCode code, std::size_t route_index, std::string_view field,
                                std::string message) {
    std::string path = "routes[" + std::to_string(route_index) + "].upstream_tls";
    if (!field.empty()) {
        path.push_back('.');
        path.append(field);
    }
    return AccessConfigError{
            .code = code,
            .field = std::move(path),
            .message = std::move(message),
    };
}

bool valid_dns_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 253 || name.front() == '.' || name.back() == '.' ||
        name.find('*') != std::string_view::npos) {
        return false;
    }
    net::IpAddress ignored;
    if (net::IpAddress::parse(name, ignored)) {
        return false;
    }
    std::size_t label_size = 0;
    for (const unsigned char ch: name) {
        if (ch == '.') {
            if (label_size == 0 || label_size > 63) {
                return false;
            }
            label_size = 0;
            continue;
        }
        const bool valid =
                (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-';
        if (!valid) {
            return false;
        }
        ++label_size;
    }
    return label_size > 0 && label_size <= 63;
}

bool valid_verify_name(std::string_view name) noexcept {
    net::IpAddress ignored;
    return net::IpAddress::parse(name, ignored) || valid_dns_name(name);
}

bool ascii_hex(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

bool valid_uuid(std::string_view value) noexcept {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-' ||
        value[14] < '1' || value[14] > '8' ||
        !((value[19] >= '8' && value[19] <= '9') || (value[19] >= 'a' && value[19] <= 'b') ||
          (value[19] >= 'A' && value[19] <= 'B'))) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        if (!ascii_hex(value[index])) {
            return false;
        }
    }
    return true;
}

std::string ascii_lower(std::string_view value) {
    std::string result(value);
    for (char &ch: result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return result;
}

void append_u64(std::string &output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

void append_field(std::string &output, std::string_view value) {
    append_u64(output, value.size());
    output.append(value);
}

std::uint64_t profile_affinity(const RouteUpstreamTlsConfig &config) {
    const std::string_view ca_pem = config.ca_pem ? std::string_view(*config.ca_pem) : std::string_view{};
    const std::string_view server_name =
            config.server_name ? std::string_view(*config.server_name) : std::string_view{};
    const std::string_view verify_name =
            config.verify_name ? std::string_view(*config.verify_name) : std::string_view{};
    std::string canonical;
    canonical.reserve(40 + ca_pem.size() + server_name.size() + verify_name.size());
    canonical.append("access-server-upstream-tls-v1", 29);
    append_u64(canonical, config.generation);
    canonical.push_back(static_cast<char>(config.verification));
    append_field(canonical, ca_pem);
    append_field(canonical, server_name);
    append_field(canonical, verify_name);

    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const std::uint8_t *>(canonical.data()), canonical.size(), digest.data());
    std::uint64_t affinity = 0;
    for (std::size_t i = 0; i < sizeof(affinity); ++i) {
        affinity = (affinity << 8U) | digest[i];
    }
    return affinity == 0 ? 1 : affinity;
}

std::uint64_t identity_affinity(std::uint64_t transport_affinity,
                                const UpstreamTlsClientIdentityDigest &identity_digest) noexcept {
    std::array<std::uint8_t, sizeof(transport_affinity) + 32 + 8> canonical{};
    constexpr std::array<std::uint8_t, 8> prefix{'m', 't', 'l', 's', '-', 'v', '1', 0};
    std::copy(prefix.begin(), prefix.end(), canonical.begin());
    for (std::size_t index = 0; index < sizeof(transport_affinity); ++index) {
        canonical[prefix.size() + index] =
                static_cast<std::uint8_t>(transport_affinity >> ((sizeof(transport_affinity) - index - 1U) * 8U));
    }
    std::copy(identity_digest.begin(), identity_digest.end(), canonical.begin() + prefix.size() + 8U);
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(canonical.data(), canonical.size(), digest.data());
    std::uint64_t affinity = 0;
    for (std::size_t index = 0; index < sizeof(affinity); ++index) {
        affinity = (affinity << 8U) | digest[index];
    }
    return affinity == 0 ? 1 : affinity;
}

std::expected<std::shared_ptr<const net::TrustStore>, AccessConfigError>
make_trust_store(UpstreamTlsVerificationMode verification, std::string_view ca_pem, std::size_t route_index) {
    if (verification == UpstreamTlsVerificationMode::Inherit ||
        verification == UpstreamTlsVerificationMode::LegacyInsecure) {
        return std::shared_ptr<const net::TrustStore>{};
    }
    const net::TrustStoreOptions options = verification == UpstreamTlsVerificationMode::CustomCa
                                                   ? net::TrustStoreOptions::from_content(std::string(ca_pem))
                                                   : net::TrustStoreOptions::system();
    auto trust_store = net::TrustStore::create(options);
    if (!trust_store) {
        return std::unexpected(
                profile_error(AccessConfigErrorCode::InvalidField, route_index,
                              verification == UpstreamTlsVerificationMode::CustomCa ? "ca_pem" : "verification",
                              "failed to initialize upstream TLS trust profile"));
    }
    return std::shared_ptr<const net::TrustStore>(std::move(*trust_store));
}

} // namespace

UpstreamTlsTransportProfile::UpstreamTlsTransportProfile(UpstreamTlsTransportProfile &&other) noexcept = default;

UpstreamTlsTransportProfile &
UpstreamTlsTransportProfile::operator=(UpstreamTlsTransportProfile &&other) noexcept = default;

UpstreamTlsTransportProfile::~UpstreamTlsTransportProfile() = default;

const net::TlsCredential *UpstreamTlsTransportProfile::client_credential() const noexcept {
    return client_identity_ ? &client_identity_->credential() : nullptr;
}

std::optional<http::HttpConnectionGroupKey>
UpstreamTlsTransportProfile::connection_key(const http::HttpConnectionGroupKey &base) const noexcept {
    const http::HttpConnectionPoolAffinity affinity(pool_affinity_);
    if (base.is_ip()) {
        return http::HttpConnectionGroupKey::from_ip(base.ip_address(), base.port(), base.scheme(), affinity);
    }
    return http::HttpConnectionGroupKey::from_name(base.host_name(), base.port(), base.scheme(), affinity);
}

std::expected<UpstreamTlsTransportProfile, AccessConfigError>
compile_upstream_tls_transport_profile(const RouteUpstreamTlsConfig &config, std::size_t route_index) {
    if (config.generation == 0) {
        return std::unexpected(profile_error(AccessConfigErrorCode::OutOfRange, route_index, "generation",
                                             "generation must be a positive integer"));
    }
    const bool has_ca = config.ca_pem && !config.ca_pem->empty();
    if (config.verification == UpstreamTlsVerificationMode::CustomCa && !has_ca) {
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidCombination, route_index, "ca_pem",
                                             "CUSTOM_CA requires a non-empty CA PEM bundle"));
    }
    if (config.verification != UpstreamTlsVerificationMode::CustomCa && config.ca_pem) {
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidCombination, route_index, "ca_pem",
                                             "ca_pem is only valid with CUSTOM_CA"));
    }
    if (config.server_name && !valid_dns_name(*config.server_name)) {
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidField, route_index, "server_name",
                                             "server_name must be a non-empty ASCII DNS name"));
    }
    if (config.verify_name && !valid_verify_name(*config.verify_name)) {
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidField, route_index, "verify_name",
                                             "verify_name must be a non-empty ASCII DNS name or IP address"));
    }
    if (config.verification == UpstreamTlsVerificationMode::LegacyInsecure && config.verify_name) {
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidCombination, route_index, "verify_name",
                                             "verify_name cannot be used with LEGACY_INSECURE"));
    }
    if (config.client_identity_ref && !valid_uuid(*config.client_identity_ref)) {
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidField, route_index, "client_identity_ref",
                                             "client_identity_ref must be a valid UUID"));
    }

    UpstreamTlsTransportProfile result;
    result.generation_ = config.generation;
    result.verification_ = config.verification;
    result.pool_affinity_ = profile_affinity(config);
    if (config.server_name) {
        result.server_name_ = *config.server_name;
    }
    if (config.verify_name) {
        result.verify_name_ = *config.verify_name;
    }
    if (config.client_identity_ref) {
        result.client_identity_ref_ = ascii_lower(*config.client_identity_ref);
    }
    auto trust_store = make_trust_store(config.verification,
                                        has_ca ? std::string_view(*config.ca_pem) : std::string_view{}, route_index);
    if (!trust_store) {
        return std::unexpected(std::move(trust_store.error()));
    }
    result.trust_store_ = std::move(*trust_store);
    return result;
}

std::expected<void, AccessConfigError> bind_upstream_tls_client_identity(UpstreamTlsTransportProfile &profile,
                                                                         UpstreamTlsClientIdentityResolver resolver,
                                                                         std::size_t route_index) {
    if (profile.client_identity_ref_.empty()) {
        return {};
    }
    if (profile.client_identity_) {
        return {};
    }
    std::shared_ptr<const UpstreamTlsClientIdentity> identity =
            resolver ? resolver.find(resolver.context, profile.client_identity_ref_) : nullptr;
    if (!identity) {
        return std::unexpected(profile_error(AccessConfigErrorCode::MissingDependency, route_index,
                                             "client_identity_ref",
                                             "referenced upstream TLS client identity is unavailable"));
    }
    profile.pool_affinity_ = identity_affinity(profile.pool_affinity_, identity->digest());
    profile.client_identity_ = std::move(identity);
    return {};
}

} // namespace fiber::access_server
