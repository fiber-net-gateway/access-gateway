#ifndef FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_IDENTITY_H
#define FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_IDENTITY_H

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

using UpstreamTlsClientIdentityDigest = std::array<std::uint8_t, 32>;

// Immutable client certificate material shared by TLS certificate and route
// snapshots. PEM bytes live only in sealed in-memory files; snapshots retain
// descriptors without retaining plaintext string copies.
class UpstreamTlsClientIdentity final : public common::NonCopyable, public common::NonMovable {
public:
    ~UpstreamTlsClientIdentity();

    [[nodiscard]] static std::expected<std::shared_ptr<const UpstreamTlsClientIdentity>, common::IoErr>
    create(std::string_view certificate_pem, std::string_view private_key_pem) noexcept;

    [[nodiscard]] std::string_view certificate_path() const noexcept { return certificate_path_; }
    [[nodiscard]] std::string_view private_key_path() const noexcept { return private_key_path_; }
    [[nodiscard]] const UpstreamTlsClientIdentityDigest &digest() const noexcept { return digest_; }

private:
    UpstreamTlsClientIdentity(int certificate_fd, int private_key_fd, UpstreamTlsClientIdentityDigest digest) noexcept;

    int certificate_fd_ = -1;
    int private_key_fd_ = -1;
    std::string certificate_path_;
    std::string private_key_path_;
    UpstreamTlsClientIdentityDigest digest_{};
};

struct UpstreamTlsClientIdentityResolver {
    using FindFunction = std::shared_ptr<const UpstreamTlsClientIdentity> (*)(void *context,
                                                                              std::string_view id) noexcept;

    void *context = nullptr;
    FindFunction find = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return find != nullptr; }
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_IDENTITY_H
