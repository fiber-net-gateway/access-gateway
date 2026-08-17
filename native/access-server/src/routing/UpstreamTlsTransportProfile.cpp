#include "UpstreamTlsTransportProfile.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <openssl/sha.h>

#include <fiber/net/IpAddress.h>
#include <fiber/net/TlsContext.h>

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

} // namespace

class UpstreamTlsCaBundle final {
public:
    explicit UpstreamTlsCaBundle(int fd) : fd_(fd), path_("/proc/self/fd/" + std::to_string(fd)) {}
    ~UpstreamTlsCaBundle() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    UpstreamTlsCaBundle(const UpstreamTlsCaBundle &) = delete;
    UpstreamTlsCaBundle &operator=(const UpstreamTlsCaBundle &) = delete;

    [[nodiscard]] std::string_view path() const noexcept { return path_; }

private:
    int fd_ = -1;
    std::string path_;
};

namespace {

std::expected<std::shared_ptr<const UpstreamTlsCaBundle>, AccessConfigError> seal_ca_bundle(std::string_view pem,
                                                                                            std::size_t route_index) {
    const int fd = memfd_create("access-server-upstream-ca", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidField, route_index, "ca_pem",
                                             "failed to create sealed upstream CA bundle"));
    }
    if (fchmod(fd, S_IRUSR) < 0) {
        close(fd);
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidField, route_index, "ca_pem",
                                             "failed to protect upstream CA bundle"));
    }
    std::size_t offset = 0;
    while (offset < pem.size()) {
        const ssize_t written = write(fd, pem.data() + offset, pem.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            close(fd);
            return std::unexpected(profile_error(AccessConfigErrorCode::InvalidField, route_index, "ca_pem",
                                                 "failed to prepare upstream CA bundle"));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) {
        close(fd);
        return std::unexpected(profile_error(AccessConfigErrorCode::InvalidField, route_index, "ca_pem",
                                             "failed to seal upstream CA bundle"));
    }
    auto *allocated = new (std::nothrow) UpstreamTlsCaBundle(fd);
    if (!allocated) {
        close(fd);
        return std::unexpected(profile_error(AccessConfigErrorCode::LimitExceeded, route_index, "ca_pem",
                                             "failed to allocate upstream CA bundle"));
    }
    std::shared_ptr<const UpstreamTlsCaBundle> bundle(allocated);
    return bundle;
}

std::expected<void, AccessConfigError> validate_trust(UpstreamTlsVerificationMode verification,
                                                      std::string_view ca_file, std::size_t route_index) {
    if (verification == UpstreamTlsVerificationMode::Inherit ||
        verification == UpstreamTlsVerificationMode::LegacyInsecure) {
        return {};
    }
    net::TlsOptions options;
    options.enabled = true;
    options.verify_peer = true;
    if (verification == UpstreamTlsVerificationMode::CustomCa) {
        options.ca_file = ca_file;
    }
    net::TlsContext context(std::move(options), false, false);
    auto initialized = context.init();
    if (!initialized) {
        return std::unexpected(
                profile_error(AccessConfigErrorCode::InvalidField, route_index,
                              verification == UpstreamTlsVerificationMode::CustomCa ? "ca_pem" : "verification",
                              "failed to initialize upstream TLS trust profile"));
    }
    return {};
}

} // namespace

UpstreamTlsTransportProfile::UpstreamTlsTransportProfile(UpstreamTlsTransportProfile &&other) noexcept = default;

UpstreamTlsTransportProfile &
UpstreamTlsTransportProfile::operator=(UpstreamTlsTransportProfile &&other) noexcept = default;

UpstreamTlsTransportProfile::~UpstreamTlsTransportProfile() = default;

std::string_view UpstreamTlsTransportProfile::ca_file() const noexcept {
    return ca_bundle_ ? ca_bundle_->path() : std::string_view{};
}

std::optional<http::Http1ConnectionGroupKey>
UpstreamTlsTransportProfile::connection_key(const http::Http1ConnectionGroupKey &base) const noexcept {
    const http::Http1ConnectionPoolAffinity affinity(pool_affinity_);
    if (base.is_ip()) {
        return http::Http1ConnectionGroupKey::from_ip(base.ip_address(), base.port(), base.scheme(), affinity);
    }
    return http::Http1ConnectionGroupKey::from_name(base.host_name(), base.port(), base.scheme(), affinity);
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
    if (has_ca) {
        auto sealed = seal_ca_bundle(*config.ca_pem, route_index);
        if (!sealed) {
            return std::unexpected(std::move(sealed.error()));
        }
        result.ca_bundle_ = std::move(*sealed);
    }
    auto trusted = validate_trust(config.verification, result.ca_file(), route_index);
    if (!trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    return result;
}

} // namespace fiber::access_server
