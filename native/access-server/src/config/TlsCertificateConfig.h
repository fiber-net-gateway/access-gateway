#ifndef FIBER_ACCESS_SERVER_TLS_CERTIFICATE_CONFIG_H
#define FIBER_ACCESS_SERVER_TLS_CERTIFICATE_CONFIG_H

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

inline constexpr std::string_view kTlsCertificatesDataId = "ploto.unified-access.tls-certificates";
inline constexpr std::string_view kTlsCertificatesGroup = "ACCESS-SERVER";
inline constexpr std::size_t kMaxTlsSnapshotBytes = 4U << 20U;
inline constexpr std::size_t kMaxTlsCertificates = 128;
inline constexpr std::size_t kMaxTlsCertificatePemBytes = 128U << 10U;
inline constexpr std::size_t kMaxTlsPrivateKeyPemBytes = 32U << 10U;
inline constexpr std::size_t kMaxTlsDnsNamesPerCertificate = 64;
inline constexpr std::size_t kMaxTlsDnsNames = 8192;

struct TlsCertificateConfig {
    std::string id;
    std::string certificate_pem;
    std::string private_key_pem;
};

struct TlsCertificateSnapshotConfig {
    std::uint64_t version = 0;
    std::string default_certificate;
    std::vector<TlsCertificateConfig> certificates;
};

enum class TlsCertificateConfigErrorCode : std::uint8_t {
    InvalidJson,
    InvalidRoot,
    InvalidField,
    MissingField,
    DuplicateField,
    LimitExceeded,
    InvalidCertificate,
    InvalidPrivateKey,
    InvalidDnsName,
    DuplicateDnsName,
    DefaultCertificateNotFound,
    VersionConflict,
};

struct TlsCertificateConfigError {
    TlsCertificateConfigErrorCode code = TlsCertificateConfigErrorCode::InvalidField;
    std::string field;
    std::string message;
    std::size_t offset = 0;
};

using TlsCertificateConfigResult =
        std::expected<std::optional<TlsCertificateSnapshotConfig>, TlsCertificateConfigError>;

// Empty content and JSON null retain the last valid snapshot. Unknown fields
// and duplicate fields are rejected so the wire contract cannot drift silently.
[[nodiscard]] TlsCertificateConfigResult parse_tls_certificate_config(std::string_view content);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TLS_CERTIFICATE_CONFIG_H
