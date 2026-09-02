#include "UpstreamTlsClientIdentity.h"

#include <new>
#include <utility>

#include <openssl/evp.h>

namespace fiber::access_server {
namespace {

bool add_u64(EVP_MD_CTX &context, std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::uint8_t>(value >> ((encoded.size() - index - 1U) * 8U));
    }
    return EVP_DigestUpdate(&context, encoded.data(), encoded.size()) == 1;
}

std::expected<UpstreamTlsClientIdentityDigest, common::IoErr>
identity_digest(std::string_view certificate_pem, std::string_view private_key_pem) noexcept {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context) {
        return std::unexpected(common::IoErr::NoMem);
    }
    UpstreamTlsClientIdentityDigest result{};
    unsigned int result_size = 0;
    const bool success = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                         add_u64(*context, certificate_pem.size()) &&
                         EVP_DigestUpdate(context, certificate_pem.data(), certificate_pem.size()) == 1 &&
                         add_u64(*context, private_key_pem.size()) &&
                         EVP_DigestUpdate(context, private_key_pem.data(), private_key_pem.size()) == 1 &&
                         EVP_DigestFinal_ex(context, result.data(), &result_size) == 1 && result_size == result.size();
    EVP_MD_CTX_free(context);
    if (!success) {
        return std::unexpected(common::IoErr::Unknown);
    }
    return result;
}

} // namespace

UpstreamTlsClientIdentity::UpstreamTlsClientIdentity(std::unique_ptr<net::TlsCredential> credential,
                                                     UpstreamTlsClientIdentityDigest digest) noexcept :
    credential_(std::move(credential)), digest_(digest) {}

UpstreamTlsClientIdentity::~UpstreamTlsClientIdentity() = default;

std::expected<std::shared_ptr<const UpstreamTlsClientIdentity>, common::IoErr>
UpstreamTlsClientIdentity::create(std::string_view certificate_pem, std::string_view private_key_pem) noexcept {
    auto digest = identity_digest(certificate_pem, private_key_pem);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = net::TlsPemSource::from_content(std::string(certificate_pem));
    credential_options.private_key = net::TlsPemSource::from_content(std::string(private_key_pem));
    auto credential = net::TlsCredential::create(credential_options);
    if (!credential) {
        return std::unexpected(credential.error());
    }
    auto *identity = new (std::nothrow) UpstreamTlsClientIdentity(std::move(*credential), *digest);
    if (!identity) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return std::shared_ptr<const UpstreamTlsClientIdentity>(identity);
}

} // namespace fiber::access_server
