#include "AccessConfigCoercion.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace fiber::access_server::config_detail {
namespace {

bool equals_ascii_case(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        unsigned char left = static_cast<unsigned char>(lhs[i]);
        unsigned char right = static_cast<unsigned char>(rhs[i]);
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<unsigned char>(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<unsigned char>(right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

template<typename T>
bool parse_signed_decimal(std::string_view value, T &out) noexcept {
    static_assert(std::is_signed_v<T>);
    if (value.empty()) {
        return false;
    }
    const char *begin = value.data();
    const char *end = begin + value.size();
    bool positive_sign = false;
    if (*begin == '+') {
        positive_sign = true;
        ++begin;
        if (begin == end) {
            return false;
        }
    }
    T result = 0;
    const auto conversion = std::from_chars(begin, end, result);
    if (conversion.ec != std::errc() || conversion.ptr != end) {
        return false;
    }
    if (positive_sign && result < 0) {
        return false;
    }
    out = result;
    return true;
}

template<typename T>
bool parse_unsigned_decimal(std::string_view value, T &out) noexcept {
    static_assert(std::is_unsigned_v<T>);
    if (value.empty()) {
        return false;
    }
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), out);
    return conversion.ec == std::errc() && conversion.ptr == value.data() + value.size();
}

void set_string_entry(StringConfigMap &entries, std::string name, std::optional<std::string> value) {
    for (StringConfigEntry &entry: entries) {
        if (entry.name == name) {
            entry.value = std::move(value);
            return;
        }
    }
    entries.push_back(StringConfigEntry{.name = std::move(name), .value = std::move(value)});
}

} // namespace

std::string_view trim_java(std::string_view value) noexcept {
    while (!value.empty() && static_cast<unsigned char>(value.front()) <= 0x20U) {
        value.remove_prefix(1);
    }
    while (!value.empty() && static_cast<unsigned char>(value.back()) <= 0x20U) {
        value.remove_suffix(1);
    }
    return value;
}

DecodeResult<std::string> java_string(const AccessJsonValue &value, std::string_view field) {
    if (value.is_text()) {
        return std::string(value.as_text());
    }
    if (value.is_integer()) {
        std::array<char, 32> buffer{};
        const auto conversion = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.as_integer());
        return std::string(buffer.data(), conversion.ptr);
    }
    if (value.is_bool()) {
        return std::string(value.as_bool() ? "true" : "false");
    }
    if (value.is_double()) {
        // Jackson returns the original numeric token when coercing a floating
        // point value to String, including exponent spelling and trailing .0.
        return std::string(value.number_text());
    }
    if (value.is_big_number()) {
        return std::string(value.number_text());
    }
    return invalid_field(field, "expected a scalar string value or null");
}

DecodeResult<std::optional<std::string>> nullable_java_string(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::string>{};
    }
    auto decoded = java_string(value, field);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return std::optional<std::string>(std::move(*decoded));
}

DecodeResult<std::int32_t> java_int32(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return 0;
    }
    if (value.is_integer()) {
        const std::int64_t integer = value.as_integer();
        if (integer < std::numeric_limits<std::int32_t>::min() || integer > std::numeric_limits<std::int32_t>::max()) {
            return out_of_range(field, "integer exceeds Java int range");
        }
        return static_cast<std::int32_t>(integer);
    }
    if (value.is_text()) {
        std::int32_t integer = 0;
        if (!parse_signed_decimal(trim_java(value.as_text()), integer)) {
            return invalid_field(field, "expected Java int");
        }
        return integer;
    }
    if (value.is_double()) {
        const double number = value.as_double();
        if (number < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            number > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            return out_of_range(field, "number exceeds Java int range");
        }
        return static_cast<std::int32_t>(number);
    }
    return invalid_field(field, "expected Java int");
}

DecodeResult<std::optional<bool>> nullable_java_bool(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<bool>{};
    }
    if (value.is_bool()) {
        return std::optional<bool>(value.as_bool());
    }
    if (value.is_integer()) {
        return std::optional<bool>(value.as_integer() != 0);
    }
    if (value.is_text()) {
        const std::string_view text = trim_java(value.as_text());
        if (text.empty()) {
            return std::optional<bool>{};
        }
        if (equals_ascii_case(text, "true")) {
            return std::optional<bool>(true);
        }
        if (equals_ascii_case(text, "false")) {
            return std::optional<bool>(false);
        }
    }
    return invalid_field(field, "expected Java Boolean");
}

DecodeResult<ResponseGzipConfig> response_gzip(const AccessJsonValue &value, std::string_view field) {
    if (value.is_bool()) {
        return ResponseGzipConfig{
                .enabled = value.as_bool(),
                .level = kDefaultGzipLevel,
        };
    }
    if (value.is_integer()) {
        const std::int64_t level = value.as_integer();
        if (level < 1 || level > 9) {
            return out_of_range(field, "gzip level must be between 1 and 9");
        }
        return ResponseGzipConfig{
                .enabled = true,
                .level = static_cast<std::uint8_t>(level),
        };
    }
    return invalid_field(field, "gzip must be a boolean or an integer level between 1 and 9");
}

DecodeResult<std::optional<std::int64_t>> duration_millis(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::int64_t>{};
    }
    if (value.is_integer()) {
        return std::optional<std::int64_t>(value.as_integer());
    }
    if (!value.is_text()) {
        return invalid_field(field, "unsupported duration token");
    }

    const std::string_view text = trim_java(value.as_text());
    if (text.empty()) {
        return std::optional<std::int64_t>{};
    }

    std::size_t digit_end = 0;
    while (digit_end < text.size() && text[digit_end] >= '0' && text[digit_end] <= '9') {
        ++digit_end;
    }
    if (digit_end == 0) {
        return invalid_field(field, "unsupported duration");
    }

    const std::string_view suffix = text.substr(digit_end);
    if (!suffix.empty() && !equals_ascii_case(suffix, "ms") && !equals_ascii_case(suffix, "s")) {
        return invalid_field(field, "unsupported duration");
    }

    std::uint32_t raw = 0;
    if (!parse_unsigned_decimal(text.substr(0, digit_end), raw) ||
        raw > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return out_of_range(field, "duration number exceeds Java int range");
    }

    std::uint32_t millis_bits = raw;
    if (equals_ascii_case(suffix, "s")) {
        // DurationDeserializer multiplies Java ints before widening, so the
        // two's-complement overflow is part of the accepted wire behavior.
        millis_bits *= 1000U;
    }
    const auto millis = std::bit_cast<std::int32_t>(millis_bits);
    return std::optional<std::int64_t>(millis);
}

DecodeResult<std::optional<std::int64_t>> data_size(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::int64_t>{};
    }
    if (value.is_integer()) {
        return std::optional<std::int64_t>(value.as_integer());
    }
    if (!value.is_text()) {
        return invalid_field(field, "unsupported data size token");
    }

    const std::string_view text = trim_java(value.as_text());
    std::size_t digit_end = 0;
    while (digit_end < text.size() && text[digit_end] >= '0' && text[digit_end] <= '9') {
        ++digit_end;
    }
    if (digit_end == 0 || text.size() - digit_end > 1) {
        return invalid_field(field, "unsupported data size");
    }

    unsigned int shift = 0;
    if (digit_end != text.size()) {
        switch (text[digit_end]) {
            case 'k':
            case 'K':
                shift = 10;
                break;
            case 'm':
            case 'M':
                shift = 20;
                break;
            case 'g':
            case 'G':
                shift = 30;
                break;
            default:
                return invalid_field(field, "unsupported data size");
        }
    }

    std::uint64_t raw = 0;
    if (!parse_unsigned_decimal(text.substr(0, digit_end), raw) ||
        raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return out_of_range(field, "data size number exceeds Java long range");
    }
    const std::uint64_t shifted = raw << shift;
    const std::int64_t result = std::bit_cast<std::int64_t>(shifted);
    if (result <= 0) {
        return invalid_field(field, "data size string must produce a positive value");
    }
    return std::optional<std::int64_t>(result);
}

DecodeResult<StringConfigMap> string_map(const AccessJsonValue &value, std::string_view field,
                                         const ProjectRouteLimits &limits) {
    if (value.is_null()) {
        return StringConfigMap{};
    }
    if (!value.is_object()) {
        return invalid_field(field, "expected object or null");
    }
    if (value.as_object().size() > limits.max_header_entries) {
        return limit_exceeded(field, "entry count exceeds the configured limit");
    }

    StringConfigMap result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        auto decoded = nullable_java_string(entry.value, path);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        set_string_entry(result, std::string(entry.key), std::move(*decoded));
    }
    return result;
}

DecodeResult<NullableStringSet> string_set(const AccessJsonValue &value, std::string_view field,
                                           std::size_t max_items) {
    if (value.is_null()) {
        return NullableStringSet{};
    }
    if (!value.is_array()) {
        return invalid_field(field, "expected array or null");
    }

    NullableStringSet result;
    const json::JsonArray<AccessJsonValue> &array = value.as_array();
    if (array.size() > max_items) {
        return limit_exceeded(field, "entry count exceeds the configured limit");
    }
    result.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        auto decoded = nullable_java_string(array[i], index_path(field, i));
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        if (std::find(result.begin(), result.end(), *decoded) == result.end()) {
            result.push_back(std::move(*decoded));
        }
    }
    return result;
}

} // namespace fiber::access_server::config_detail
