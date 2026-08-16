#ifndef FIBER_ACCESS_SERVER_ACCESS_LOG_POLICY_H
#define FIBER_ACCESS_SERVER_ACCESS_LOG_POLICY_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/http/HttpCommon.h>

namespace fiber::access_server {

inline constexpr std::uint32_t kAccessLogSampleScale = 10000;
inline constexpr std::size_t kMinAccessLogFieldBytes = 16;
inline constexpr std::size_t kMaxAccessLogFieldBytes = 65536;
inline constexpr std::size_t kMaxAccessLogQueryKeys = 64;
inline constexpr std::size_t kMaxAccessLogQueryKeyBytes = 128;

struct AccessLogOptions {
    std::vector<std::string> query_allowlist;
    std::vector<std::string> additional_sensitive_query_keys;
    bool query_hash_enabled = false;
    std::uint32_t success_sample_rate_bps = kAccessLogSampleScale;
    std::size_t max_path_bytes = 2048;
    std::size_t max_query_bytes = 2048;
};

struct AccessLogUri {
    // The common safe/non-truncated path borrows HttpUri storage. Encoded paths use path_storage so ordinary
    // requests do not add an allocation solely for access logging.
    std::string_view borrowed_path;
    std::string path_storage;
    std::string query;
    std::string query_hash;
    bool path_truncated = false;
    bool query_filtered = false;
    bool query_redacted = false;
    bool query_truncated = false;
    bool query_hash_failed = false;

    [[nodiscard]] std::string_view path() const noexcept {
        return path_storage.empty() ? borrowed_path : std::string_view(path_storage);
    }
};

// Immutable request-log policy. Configuration is compiled once at server construction; request paths only perform
// bounded rendering and an optional HMAC for records that survive sampling.
class AccessLogPolicy final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessLogPolicy(AccessLogOptions options = {});
    ~AccessLogPolicy();

    [[nodiscard]] common::IoResult<void> initialize() noexcept;
    [[nodiscard]] AccessLogUri render_uri(const http::HttpUri &uri) const;
    [[nodiscard]] bool should_log(bool failed, std::uint32_t sample) const noexcept;
    [[nodiscard]] const AccessLogOptions &options() const noexcept { return options_; }

private:
    [[nodiscard]] bool allowlisted(std::string_view key) const noexcept;
    [[nodiscard]] bool sensitive(std::string_view key) const noexcept;
    [[nodiscard]] std::string query_hash(std::string_view query) const noexcept;

    AccessLogOptions options_;
    std::vector<std::string> sensitive_query_keys_;
    std::array<std::uint8_t, 32> query_hash_key_{};
    std::atomic_bool query_hash_ready_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_LOG_POLICY_H
