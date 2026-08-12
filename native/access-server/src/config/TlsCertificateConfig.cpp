#include "TlsCertificateConfig.h"

#include <limits>
#include <unordered_set>
#include <utility>

#include <fiber/common/json/JsonParse.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server {
namespace {

using json::JsonAny;
using json::JsonObject;

TlsCertificateConfigError make_error(TlsCertificateConfigErrorCode code, std::string field, std::string message,
                                     std::size_t offset = 0) {
    return TlsCertificateConfigError{
            .code = code,
            .field = std::move(field),
            .message = std::move(message),
            .offset = offset,
    };
}

const JsonAny *unique_field(const JsonObject<JsonAny> &object, std::string_view name,
                            TlsCertificateConfigError &error) {
    const JsonAny *result = nullptr;
    for (const auto &entry: object) {
        if (entry.key != name) {
            continue;
        }
        if (result) {
            error = make_error(TlsCertificateConfigErrorCode::DuplicateField, std::string(name),
                               "field must appear exactly once");
            return nullptr;
        }
        result = &entry.value;
    }
    return result;
}

bool reject_unknown_fields(const JsonObject<JsonAny> &object, std::initializer_list<std::string_view> allowed,
                           std::string_view prefix, TlsCertificateConfigError &error) {
    for (const auto &entry: object) {
        bool known = false;
        for (std::string_view name: allowed) {
            if (entry.key == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            std::string field(prefix);
            field.append(entry.key);
            error = make_error(TlsCertificateConfigErrorCode::InvalidField, std::move(field), "unknown field");
            return false;
        }
    }
    return true;
}

std::expected<TlsCertificateConfig, TlsCertificateConfigError> parse_certificate(const JsonAny &value,
                                                                                 std::size_t index) {
    const std::string prefix = "certificates[" + std::to_string(index) + "].";
    if (!value.is_object()) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::InvalidField,
                                          "certificates[" + std::to_string(index) + "]",
                                          "certificate entry must be an object"));
    }
    const auto &object = value.as_object();
    TlsCertificateConfigError field_error;
    if (!reject_unknown_fields(object, {"id", "certificatePem", "privateKeyPem"}, prefix, field_error)) {
        return std::unexpected(std::move(field_error));
    }
    const JsonAny *id = unique_field(object, "id", field_error);
    if (!id) {
        if (field_error.message.empty()) {
            field_error = make_error(TlsCertificateConfigErrorCode::MissingField, prefix + "id", "field is required");
        }
        return std::unexpected(std::move(field_error));
    }
    const JsonAny *certificate = unique_field(object, "certificatePem", field_error);
    if (!certificate) {
        if (field_error.message.empty()) {
            field_error = make_error(TlsCertificateConfigErrorCode::MissingField, prefix + "certificatePem",
                                     "field is required");
        }
        return std::unexpected(std::move(field_error));
    }
    const JsonAny *private_key = unique_field(object, "privateKeyPem", field_error);
    if (!private_key) {
        if (field_error.message.empty()) {
            field_error = make_error(TlsCertificateConfigErrorCode::MissingField, prefix + "privateKeyPem",
                                     "field is required");
        }
        return std::unexpected(std::move(field_error));
    }
    if (!id->is_text() || id->as_text().empty() || id->as_text().size() > 128) {
        return std::unexpected(
                make_error(TlsCertificateConfigErrorCode::InvalidField, prefix + "id", "id must be 1-128 bytes"));
    }
    if (!certificate->is_text() || certificate->as_text().empty() ||
        certificate->as_text().size() > kMaxTlsCertificatePemBytes) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::LimitExceeded, prefix + "certificatePem",
                                          "certificate PEM must be 1-131072 bytes"));
    }
    if (!private_key->is_text() || private_key->as_text().empty() ||
        private_key->as_text().size() > kMaxTlsPrivateKeyPemBytes) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::LimitExceeded, prefix + "privateKeyPem",
                                          "private key PEM must be 1-32768 bytes"));
    }
    return TlsCertificateConfig{
            .id = std::string(id->as_text()),
            .certificate_pem = std::string(certificate->as_text()),
            .private_key_pem = std::string(private_key->as_text()),
    };
}

} // namespace

TlsCertificateConfigResult parse_tls_certificate_config(std::string_view content) {
    if (content.empty()) {
        return std::optional<TlsCertificateSnapshotConfig>{};
    }
    if (content.size() > kMaxTlsSnapshotBytes) {
        return std::unexpected(
                make_error(TlsCertificateConfigErrorCode::LimitExceeded, {}, "TLS certificate snapshot exceeds 4 MiB"));
    }

    mem::BufPool pool;
    json::JsonParser parser;
    if (!parser.feed(content.data(), content.size())) {
        const auto &parse_error = parser.error();
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::InvalidJson, {},
                                          parse_error.message ? parse_error.message : "invalid JSON",
                                          parse_error.offset));
    }
    parser.finish();
    JsonAny root;
    if (json::parse_document<json::parse_any>(parser, pool, root) != json::ParseStatus::Done) {
        const auto &parse_error = parser.error();
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::InvalidJson, {},
                                          parse_error.message ? parse_error.message : "invalid JSON",
                                          parse_error.offset));
    }
    if (root.is_null()) {
        return std::optional<TlsCertificateSnapshotConfig>{};
    }
    if (!root.is_object()) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::InvalidRoot, {},
                                          "TLS certificate snapshot must be an object or null"));
    }

    const auto &object = root.as_object();
    TlsCertificateConfigError field_error;
    if (!reject_unknown_fields(object, {"schemaVersion", "version", "defaultCertificate", "certificates"}, {},
                               field_error)) {
        return std::unexpected(std::move(field_error));
    }
    const JsonAny *schema_version = unique_field(object, "schemaVersion", field_error);
    const JsonAny *version = unique_field(object, "version", field_error);
    const JsonAny *default_certificate = unique_field(object, "defaultCertificate", field_error);
    const JsonAny *certificates = unique_field(object, "certificates", field_error);
    if (!field_error.message.empty()) {
        return std::unexpected(std::move(field_error));
    }
    if (!schema_version || !version || !default_certificate || !certificates) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::MissingField, {},
                                          "schemaVersion, version, defaultCertificate and "
                                          "certificates are required"));
    }
    if (!schema_version->is_integer() || schema_version->as_integer() != 1) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::InvalidField, "schemaVersion",
                                          "only schemaVersion 1 is supported"));
    }
    if (!version->is_integer() || version->as_integer() <= 0) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::InvalidField, "version",
                                          "version must be a positive integer"));
    }
    if (!default_certificate->is_text() || default_certificate->as_text().empty() ||
        default_certificate->as_text().size() > 128) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::InvalidField, "defaultCertificate",
                                          "defaultCertificate must be 1-128 bytes"));
    }
    if (!certificates->is_array() || certificates->as_array().empty() ||
        certificates->as_array().size() > kMaxTlsCertificates) {
        return std::unexpected(make_error(TlsCertificateConfigErrorCode::LimitExceeded, "certificates",
                                          "certificates must contain 1-128 entries"));
    }

    TlsCertificateSnapshotConfig result{
            .version = static_cast<std::uint64_t>(version->as_integer()),
            .default_certificate = std::string(default_certificate->as_text()),
    };
    result.certificates.reserve(certificates->as_array().size());
    std::unordered_set<std::string> ids;
    for (std::size_t i = 0; i < certificates->as_array().size(); ++i) {
        auto certificate = parse_certificate(certificates->as_array()[i], i);
        if (!certificate) {
            return std::unexpected(std::move(certificate.error()));
        }
        if (!ids.emplace(certificate->id).second) {
            return std::unexpected(make_error(TlsCertificateConfigErrorCode::DuplicateField,
                                              "certificates[" + std::to_string(i) + "].id",
                                              "certificate id is duplicated"));
        }
        result.certificates.push_back(std::move(*certificate));
    }
    return std::optional<TlsCertificateSnapshotConfig>(std::move(result));
}

} // namespace fiber::access_server
