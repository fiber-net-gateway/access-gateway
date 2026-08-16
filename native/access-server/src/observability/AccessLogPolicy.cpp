#include "AccessLogPolicy.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

constexpr auto kSensitiveQueryKeys = std::to_array<std::string_view>({
        "access_token",  "api-key", "api_key",    "apikey",      "assertion", "auth",          "authorization",
        "client_secret", "code",    "credential", "csrf_token",  "id_token",  "jwt",           "oauth_token",
        "passcode",      "passwd",  "password",   "private_key", "pwd",       "refresh_token", "saml_response",
        "samlrequest",   "secret",  "session",    "session_id",  "sessionid", "sig",           "signature",
        "ticket",        "token",   "x-api-key",  "xsrf_token",
});
constexpr std::string_view kRedactedValue = "[REDACTED]";
constexpr std::size_t kEncodedObservedKeyBytes = kMaxAccessLogQueryKeyBytes * 3;

unsigned char hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned char>(value - 'A' + 10);
    }
    return 0xff;
}

char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

void ascii_lower_in_place(std::string &value) noexcept {
    for (char &character: value) {
        character = ascii_lower(character);
    }
}

bool decode_query_key(std::string_view input, std::array<char, kMaxAccessLogQueryKeyBytes> &storage,
                      std::string_view &output) noexcept {
    std::size_t written = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        unsigned char value = static_cast<unsigned char>(input[index]);
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (index + 2 >= input.size()) {
                return false;
            }
            const unsigned char high = hex_digit(input[index + 1]);
            const unsigned char low = hex_digit(input[index + 2]);
            if (high == 0xff || low == 0xff) {
                return false;
            }
            value = static_cast<unsigned char>((high << 4U) | low);
            index += 2;
        }
        // Configured keys are restricted to ASCII. Rejecting other bytes avoids request-time allocation and cannot
        // hide a key that could have matched the allowlist.
        if (value >= 0x80 || written == storage.size()) {
            return false;
        }
        storage[written++] = static_cast<char>(value);
    }
    output = std::string_view(storage.data(), written);
    return !output.empty();
}

bool safe_path_byte(unsigned char value) noexcept {
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')) {
        return true;
    }
    switch (value) {
        case '-':
        case '.':
        case '_':
        case '~':
        case '/':
        case ':':
        case '@':
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case '%':
            return true;
        default:
            return false;
    }
}

bool safe_query_byte(unsigned char value) noexcept {
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')) {
        return true;
    }
    switch (value) {
        case '-':
        case '.':
        case '_':
        case '~':
        case '%':
        case '+':
        case '/':
        case '?':
        case ':':
        case '@':
        case '!':
        case '$':
        case '\'':
        case '(':
        case ')':
        case '*':
        case ',':
        case ';':
            return true;
        default:
            return false;
    }
}

template<typename Predicate>
std::size_t encoded_size(std::string_view input, std::size_t maximum, Predicate safe) noexcept {
    std::size_t size = 0;
    for (unsigned char value: input) {
        const std::size_t width = safe(value) ? 1 : 3;
        if (width > maximum - std::min(size, maximum)) {
            return maximum == std::numeric_limits<std::size_t>::max() ? maximum : maximum + 1;
        }
        size += width;
    }
    return size;
}

template<typename Predicate>
void append_encoded(std::string_view input, std::string &output, std::size_t capacity, Predicate safe,
                    bool &truncated) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char value: input) {
        const std::size_t width = safe(value) ? 1 : 3;
        if (width > capacity - std::min(output.size(), capacity)) {
            truncated = true;
            return;
        }
        if (width == 1) {
            output.push_back(static_cast<char>(value));
        } else {
            output.push_back('%');
            output.push_back(kHex[value >> 4U]);
            output.push_back(kHex[value & 0x0FU]);
        }
    }
}

std::string encode_path(std::string_view path, std::size_t maximum, std::size_t size, bool &truncated) {
    if (size <= maximum) {
        std::string output;
        output.reserve(size);
        append_encoded(path, output, maximum, &safe_path_byte, truncated);
        return output;
    }

    std::string output;
    output.reserve(maximum);
    const std::size_t content_capacity = maximum - 3;
    append_encoded(path, output, content_capacity, &safe_path_byte, truncated);
    output.append("...");
    truncated = true;
    return output;
}

class BoundedQueryBuilder {
public:
    BoundedQueryBuilder(std::string &output, std::size_t maximum) : output_(&output), content_capacity_(maximum - 3) {}

    bool append_literal(std::string_view value) {
        if (value.size() > content_capacity_ - std::min(output_->size(), content_capacity_)) {
            truncated_ = true;
            return false;
        }
        ensure_capacity();
        output_->append(value);
        return true;
    }

    bool append_component(std::string_view value) {
        ensure_capacity();
        append_encoded(value, *output_, content_capacity_, &safe_query_byte, truncated_);
        return !truncated_;
    }

    void finish() {
        if (truncated_) {
            output_->append("...");
        }
    }

    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

private:
    void ensure_capacity() {
        if (output_->capacity() < std::min<std::size_t>(content_capacity_, 256)) {
            output_->reserve(std::min<std::size_t>(content_capacity_, 256));
        }
    }

    std::string *output_ = nullptr;
    std::size_t content_capacity_ = 0;
    bool truncated_ = false;
};

} // namespace

AccessLogPolicy::AccessLogPolicy(AccessLogOptions options) : options_(std::move(options)) {
    FIBER_ASSERT(options_.success_sample_rate_bps <= kAccessLogSampleScale);
    FIBER_ASSERT(options_.max_path_bytes >= kMinAccessLogFieldBytes &&
                 options_.max_path_bytes <= kMaxAccessLogFieldBytes);
    FIBER_ASSERT(options_.max_query_bytes >= kMinAccessLogFieldBytes &&
                 options_.max_query_bytes <= kMaxAccessLogFieldBytes);
    FIBER_ASSERT(options_.query_allowlist.size() <= kMaxAccessLogQueryKeys);
    FIBER_ASSERT(options_.additional_sensitive_query_keys.size() <= kMaxAccessLogQueryKeys);
    std::sort(options_.query_allowlist.begin(), options_.query_allowlist.end());
    options_.query_allowlist.erase(std::unique(options_.query_allowlist.begin(), options_.query_allowlist.end()),
                                   options_.query_allowlist.end());

    sensitive_query_keys_.reserve(kSensitiveQueryKeys.size() + options_.additional_sensitive_query_keys.size());
    for (std::string_view key: kSensitiveQueryKeys) {
        sensitive_query_keys_.emplace_back(key);
    }
    for (std::string key: options_.additional_sensitive_query_keys) {
        ascii_lower_in_place(key);
        sensitive_query_keys_.push_back(std::move(key));
    }
    std::sort(sensitive_query_keys_.begin(), sensitive_query_keys_.end());
    sensitive_query_keys_.erase(std::unique(sensitive_query_keys_.begin(), sensitive_query_keys_.end()),
                                sensitive_query_keys_.end());
}

AccessLogPolicy::~AccessLogPolicy() { OPENSSL_cleanse(query_hash_key_.data(), query_hash_key_.size()); }

common::IoResult<void> AccessLogPolicy::initialize() noexcept {
    if (!options_.query_hash_enabled || query_hash_ready_.load(std::memory_order_acquire)) {
        return {};
    }
    if (RAND_bytes(query_hash_key_.data(), static_cast<int>(query_hash_key_.size())) != 1) {
        return std::unexpected(common::IoErr::Unknown);
    }
    query_hash_ready_.store(true, std::memory_order_release);
    return {};
}

AccessLogUri AccessLogPolicy::render_uri(const http::HttpUri &uri) const {
    AccessLogUri rendered;
    const std::size_t path_size = encoded_size(uri.path, options_.max_path_bytes, &safe_path_byte);
    if (path_size == uri.path.size() && path_size <= options_.max_path_bytes) {
        rendered.borrowed_path = uri.path;
    } else {
        rendered.path_storage = encode_path(uri.path, options_.max_path_bytes, path_size, rendered.path_truncated);
    }
    if (uri.query.empty()) {
        return rendered;
    }
    if (options_.query_allowlist.empty()) {
        rendered.query_filtered = true;
        if (options_.query_hash_enabled) {
            rendered.query_hash = query_hash(uri.query);
            rendered.query_hash_failed = rendered.query_hash.empty();
        }
        return rendered;
    }

    BoundedQueryBuilder output(rendered.query, options_.max_query_bytes);
    std::array<char, kMaxAccessLogQueryKeyBytes> decoded_key_storage{};
    std::string_view decoded_key;
    bool first = true;
    std::size_t position = 0;
    while (position <= uri.query.size()) {
        const std::size_t separator = uri.query.find('&', position);
        const std::size_t end = separator == std::string_view::npos ? uri.query.size() : separator;
        const std::string_view segment = uri.query.substr(position, end - position);
        const std::size_t equals = segment.find('=');
        const std::string_view raw_key = equals == std::string_view::npos ? segment : segment.substr(0, equals);
        const std::string_view raw_value =
                equals == std::string_view::npos ? std::string_view{} : segment.substr(equals + 1);

        const bool key_decoded = raw_key.size() <= kEncodedObservedKeyBytes &&
                                 decode_query_key(raw_key, decoded_key_storage, decoded_key);
        if (!key_decoded || !allowlisted(decoded_key)) {
            rendered.query_filtered = true;
        } else {
            if ((!first && !output.append_literal("&")) || !output.append_component(decoded_key) ||
                (equals != std::string_view::npos && !output.append_literal("="))) {
                break;
            }
            first = false;
            if (sensitive(decoded_key)) {
                rendered.query_redacted = true;
                if (!output.append_literal(kRedactedValue)) {
                    break;
                }
            } else if (!output.append_component(raw_value)) {
                break;
            }
        }

        if (separator == std::string_view::npos) {
            break;
        }
        position = separator + 1;
    }
    output.finish();
    rendered.query_truncated = output.truncated();

    if (options_.query_hash_enabled) {
        rendered.query_hash = query_hash(uri.query);
        rendered.query_hash_failed = rendered.query_hash.empty();
    }
    return rendered;
}

bool AccessLogPolicy::should_log(bool failed, std::uint32_t sample) const noexcept {
    return failed || options_.success_sample_rate_bps >= kAccessLogSampleScale ||
           sample % kAccessLogSampleScale < options_.success_sample_rate_bps;
}

bool AccessLogPolicy::allowlisted(std::string_view key) const noexcept {
    const auto iterator =
            std::lower_bound(options_.query_allowlist.begin(), options_.query_allowlist.end(), key,
                             [](const std::string &left, std::string_view right) { return left < right; });
    return iterator != options_.query_allowlist.end() && *iterator == key;
}

bool AccessLogPolicy::sensitive(std::string_view key) const noexcept {
    FIBER_ASSERT(key.size() <= kMaxAccessLogQueryKeyBytes);
    std::array<char, kMaxAccessLogQueryKeyBytes> normalized_storage{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        normalized_storage[index] = ascii_lower(key[index]);
    }
    const std::string_view normalized(normalized_storage.data(), key.size());
    const auto iterator =
            std::lower_bound(sensitive_query_keys_.begin(), sensitive_query_keys_.end(), normalized,
                             [](const std::string &left, std::string_view right) { return left < right; });
    return iterator != sensitive_query_keys_.end() && *iterator == normalized;
}

std::string AccessLogPolicy::query_hash(std::string_view query) const noexcept {
    if (!query_hash_ready_.load(std::memory_order_acquire)) {
        return {};
    }
    std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    if (!HMAC(EVP_sha256(), query_hash_key_.data(), static_cast<int>(query_hash_key_.size()),
              reinterpret_cast<const std::uint8_t *>(query.data()), query.size(), digest.data(), &digest_length) ||
        digest_length != 32) {
        return {};
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string output("hmac-sha256:");
    output.reserve(output.size() + digest_length * 2);
    for (std::size_t index = 0; index < digest_length; ++index) {
        output.push_back(kHex[digest[index] >> 4U]);
        output.push_back(kHex[digest[index] & 0x0FU]);
    }
    return output;
}

} // namespace fiber::access_server
