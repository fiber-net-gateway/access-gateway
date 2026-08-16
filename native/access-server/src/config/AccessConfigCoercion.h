#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_COERCION_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_COERCION_H

#include "AccessConfig.h"
#include "AccessConfigErrorBuilder.h"
#include "AccessConfigJson.h"
#include "AccessConfigLimits.h"

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fiber::access_server::config_detail {

[[nodiscard]] std::string_view trim_java(std::string_view value) noexcept;

[[nodiscard]] DecodeResult<std::string> java_string(const AccessJsonValue &value, std::string_view field);
[[nodiscard]] DecodeResult<std::optional<std::string>> nullable_java_string(const AccessJsonValue &value,
                                                                            std::string_view field);
[[nodiscard]] DecodeResult<std::int32_t> java_int32(const AccessJsonValue &value, std::string_view field);
[[nodiscard]] DecodeResult<std::optional<bool>> nullable_java_bool(const AccessJsonValue &value,
                                                                   std::string_view field);
[[nodiscard]] DecodeResult<ResponseGzipConfig> response_gzip(const AccessJsonValue &value, std::string_view field);

template<typename Enum>
using EnumLookup = std::initializer_list<std::pair<std::string_view, Enum>>;

template<typename Enum>
[[nodiscard]] DecodeResult<std::optional<Enum>> java_enum(const AccessJsonValue &value, std::string_view field,
                                                          EnumLookup<Enum> values) {
    if (value.is_null()) {
        return std::optional<Enum>{};
    }
    if (value.is_text()) {
        for (const auto &[name, candidate]: values) {
            if (value.as_text() == name) {
                return std::optional<Enum>(candidate);
            }
        }
        // READ_UNKNOWN_ENUM_VALUES_AS_NULL is enabled in the Java mapper.
        return std::optional<Enum>{};
    }
    if (value.is_integer()) {
        const std::int64_t ordinal = value.as_integer();
        if (ordinal < 0 || ordinal >= static_cast<std::int64_t>(values.size())) {
            return std::optional<Enum>{};
        }
        auto iterator = values.begin();
        std::advance(iterator, ordinal);
        return std::optional<Enum>(iterator->second);
    }
    return invalid_field(field, "expected enum name, ordinal, or null");
}

[[nodiscard]] DecodeResult<std::optional<std::int64_t>> duration_millis(const AccessJsonValue &value,
                                                                        std::string_view field);
[[nodiscard]] DecodeResult<std::optional<std::int64_t>> data_size(const AccessJsonValue &value, std::string_view field);
[[nodiscard]] DecodeResult<StringConfigMap> string_map(const AccessJsonValue &value, std::string_view field,
                                                       const ProjectRouteLimits &limits);
[[nodiscard]] DecodeResult<NullableStringSet> string_set(const AccessJsonValue &value, std::string_view field,
                                                         std::size_t max_items);

} // namespace fiber::access_server::config_detail

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_COERCION_H
