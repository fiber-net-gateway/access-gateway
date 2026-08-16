#include "AccessConfigCodec.h"

#include "AccessConfigCoercion.h"
#include "AccessConfigErrorBuilder.h"
#include "AccessConfigJson.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server {
namespace {

using config_detail::AccessJsonValue;
using config_detail::child_path;
using config_detail::DecodeResult;
using config_detail::invalid_field;
using config_detail::java_int32;
using config_detail::limit_exceeded;
using config_detail::string_set;

DecodeResult<GrayMatchConfig> gray_match_config(const json::JsonObject<AccessJsonValue> &object,
                                                const GrayRuleLimits &limits) {
    GrayMatchConfig result;
    if (object.size() > limits.max_rules) {
        return limit_exceeded("rules", "gray rule count exceeds the configured limit");
    }
    for (const auto &entry: object) {
        const std::string entry_path(entry.key);
        if (entry.value.is_null() || !entry.value.is_object()) {
            return invalid_field(entry_path, "expected gray-match object");
        }

        GrayMatchConfigEntry decoded_entry{
                .entry = std::string(entry.key),
        };
        for (const auto &property: entry.value.as_object()) {
            const std::string path = child_path(entry_path, property.key);
            if (property.key == "ratio") {
                auto decoded = java_int32(property.value, path);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                decoded_entry.ratio = *decoded;
            } else if (property.key == "cidrs") {
                auto decoded = string_set(property.value, path, limits.max_cidrs_per_rule);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                decoded_entry.cidrs = std::move(*decoded);
            }
        }

        const auto existing = std::find_if(result.begin(), result.end(), [&](const GrayMatchConfigEntry &current) {
            return current.entry == decoded_entry.entry;
        });
        if (existing == result.end()) {
            result.push_back(std::move(decoded_entry));
        } else {
            *existing = std::move(decoded_entry);
        }
    }
    return result;
}

} // namespace

GrayMatchConfigResult parse_gray_match_config(std::string_view content, const AccessConfigLimits &limits) {
    if (content.empty()) {
        return std::optional<GrayMatchConfig>{};
    }
    if (content.size() > limits.gray_rules.max_payload_bytes) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::LimitExceeded, "payload",
                                                         "gray rules payload exceeds the configured byte limit"));
    }

    mem::BufPool pool;
    json::JsonParser parser;
    auto root = config_detail::parse_access_json(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    if (root->is_null()) {
        return std::optional<GrayMatchConfig>(GrayMatchConfig{});
    }
    if (!root->is_object()) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::InvalidRoot, {},
                                                         "gray-match configuration must be an object or null"));
    }
    auto decoded = gray_match_config(root->as_object(), limits.gray_rules);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    auto within_limits = validate_gray_match_config_limits(*decoded, limits);
    if (!within_limits) {
        return std::unexpected(std::move(within_limits.error()));
    }
    return std::optional<GrayMatchConfig>(std::move(*decoded));
}

} // namespace fiber::access_server
