#include "UpstreamTlsClientIdentity.h"

#include <cerrno>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace fiber::access_server {
namespace {

std::expected<int, common::IoErr> seal(std::string_view name, std::string_view content) noexcept {
    const int fd = memfd_create(name.data(), MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (fchmod(fd, S_IRUSR) < 0) {
        close(fd);
        return std::unexpected(common::IoErr::Permission);
    }
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = write(fd, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            close(fd);
            return std::unexpected(common::IoErr::Unknown);
        }
        offset += static_cast<std::size_t>(written);
    }
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) {
        close(fd);
        return std::unexpected(common::IoErr::Unknown);
    }
    return fd;
}

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

UpstreamTlsClientIdentity::UpstreamTlsClientIdentity(int certificate_fd, int private_key_fd,
                                                     UpstreamTlsClientIdentityDigest digest) noexcept :
    certificate_fd_(certificate_fd), private_key_fd_(private_key_fd),
    certificate_path_("/proc/self/fd/" + std::to_string(certificate_fd)),
    private_key_path_("/proc/self/fd/" + std::to_string(private_key_fd)), digest_(digest) {}

UpstreamTlsClientIdentity::~UpstreamTlsClientIdentity() {
    if (certificate_fd_ >= 0) {
        close(certificate_fd_);
    }
    if (private_key_fd_ >= 0) {
        close(private_key_fd_);
    }
}

std::expected<std::shared_ptr<const UpstreamTlsClientIdentity>, common::IoErr>
UpstreamTlsClientIdentity::create(std::string_view certificate_pem, std::string_view private_key_pem) noexcept {
    auto digest = identity_digest(certificate_pem, private_key_pem);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    auto certificate_fd = seal("access-server-upstream-certificate", certificate_pem);
    if (!certificate_fd) {
        return std::unexpected(certificate_fd.error());
    }
    auto private_key_fd = seal("access-server-upstream-private-key", private_key_pem);
    if (!private_key_fd) {
        close(*certificate_fd);
        return std::unexpected(private_key_fd.error());
    }
    auto *identity = new (std::nothrow) UpstreamTlsClientIdentity(*certificate_fd, *private_key_fd, std::move(*digest));
    if (!identity) {
        close(*certificate_fd);
        close(*private_key_fd);
        return std::unexpected(common::IoErr::NoMem);
    }
    return std::shared_ptr<const UpstreamTlsClientIdentity>(identity);
}

} // namespace fiber::access_server
