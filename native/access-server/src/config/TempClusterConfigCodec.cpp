#include "AccessConfigCodec.h"

#include "AccessConfigCoercion.h"
#include "AccessConfigErrorBuilder.h"
#include "AccessConfigJson.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server {
namespace {

using config_detail::AccessJsonValue;
using config_detail::DecodeResult;
using config_detail::invalid_field;
using config_detail::limit_exceeded;
using config_detail::string_set;

// The whitelist is an ops-managed test-environment document, not a
// console-published resource, so it carries fixed defensive caps instead of an
// AccessConfigLimits section.
constexpr std::size_t kMaxPayloadBytes = 64U << 10U;
constexpr std::size_t kMaxServices = 256;
constexpr std::size_t kMaxClustersPerService = 64;

DecodeResult<TempClusterConfig> temp_cluster_config(const json::JsonObject<AccessJsonValue> &object) {
    TempClusterConfig result;
    if (object.size() > kMaxServices) {
        return limit_exceeded("services", "temp-cluster service count exceeds the accepted limit");
    }
    result.reserve(object.size());
    for (const auto &entry: object) {
        auto decoded = string_set(entry.value, entry.key, kMaxClustersPerService);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        TempClusterConfigEntry decoded_entry{
                .service = std::string(entry.key),
        };
        decoded_entry.clusters.reserve(decoded->size());
        for (const std::optional<std::string> &cluster: *decoded) {
            if (!cluster) {
                return invalid_field(entry.key, "temp-cluster entries must be non-null strings");
            }
            decoded_entry.clusters.push_back(std::move(*cluster));
        }
        result.push_back(std::move(decoded_entry));
    }
    return result;
}

} // namespace

TempClusterConfigResult parse_temp_cluster_config(std::string_view content) {
    // Java updateNameClusterMap maps empty or null content to an empty map, so
    // unlike the gray config an empty payload clears the whitelist.
    if (content.empty()) {
        return std::optional<TempClusterConfig>(TempClusterConfig{});
    }
    if (content.size() > kMaxPayloadBytes) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::LimitExceeded, "payload",
                                                         "temp-cluster payload exceeds the accepted byte limit"));
    }

    mem::BufPool pool;
    json::JsonParser parser;
    auto root = config_detail::parse_access_json(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    if (root->is_null()) {
        return std::optional<TempClusterConfig>(TempClusterConfig{});
    }
    if (!root->is_object()) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::InvalidRoot, {},
                                                         "temp-cluster configuration must be an object or null"));
    }
    auto decoded = temp_cluster_config(root->as_object());
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return std::optional<TempClusterConfig>(std::move(*decoded));
}

} // namespace fiber::access_server
