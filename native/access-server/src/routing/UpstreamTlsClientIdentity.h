#ifndef FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_IDENTITY_H
#define FIBER_ACCESS_SERVER_UPSTREAM_TLS_CLIENT_IDENTITY_H

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/net/TlsCredential.h>

namespace fiber::access_server {

using UpstreamTlsClientIdentityDigest = std::array<std::uint8_t, 32>;

// Immutable client certificate material shared by TLS certificate and route
// snapshots. The PEM bytes are parsed once into a BoringSSL credential; no
// plaintext file or string copy is retained afterwards.
class UpstreamTlsClientIdentity final : public common::NonCopyable, public common::NonMovable {
public:
    ~UpstreamTlsClientIdentity();

    [[nodiscard]] static std::expected<std::shared_ptr<const UpstreamTlsClientIdentity>, common::IoErr>
    create(std::string_view certificate_pem, std::string_view private_key_pem) noexcept;

    [[nodiscard]] const net::TlsCredential &credential() const noexcept { return *credential_; }
    [[nodiscard]] const UpstreamTlsClientIdentityDigest &digest() const noexcept { return digest_; }

private:
    UpstreamTlsClientIdentity(std::unique_ptr<net::TlsCredential> credential,
                              UpstreamTlsClientIdentityDigest digest) noexcept;

    std::unique_ptr<net::TlsCredential> credential_;
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
