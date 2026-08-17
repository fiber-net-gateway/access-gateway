#include "AccessConfigCodec.h"

#include "AccessConfigCoercion.h"
#include "AccessConfigErrorBuilder.h"
#include "AccessConfigJson.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server {
namespace {

using config_detail::AccessJsonValue;
using config_detail::child_path;
using config_detail::data_size;
using config_detail::DecodeResult;
using config_detail::duration_millis;
using config_detail::index_path;
using config_detail::invalid_field;
using config_detail::java_enum;
using config_detail::java_int32;
using config_detail::java_string;
using config_detail::limit_exceeded;
using config_detail::nullable_java_bool;
using config_detail::nullable_java_string;
using config_detail::response_gzip;
using config_detail::string_map;
using config_detail::string_set;

DecodeResult<std::optional<RouteBodyConfig>> route_body(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<RouteBodyConfig>{};
    }
    if (!value.is_object()) {
        return invalid_field(field, "expected body object or null");
    }

    RouteBodyConfig result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        if (entry.key == "type") {
            auto decoded = java_enum<BodyType>(
                    entry.value, path,
                    {{"TEXT", BodyType::Text}, {"BASE64", BodyType::Base64}, {"TEMPLATE", BodyType::Template}});
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.type = *decoded;
        } else if (entry.key == "content") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.content = std::move(*decoded);
        }
    }
    return std::optional<RouteBodyConfig>(std::move(result));
}

DecodeResult<std::optional<RouteUpstreamTlsConfig>> route_upstream_tls(const AccessJsonValue &value,
                                                                       std::string_view field) {
    if (value.is_null()) {
        return std::optional<RouteUpstreamTlsConfig>{};
    }
    if (!value.is_object()) {
        return invalid_field(field, "expected upstream_tls object or null");
    }

    RouteUpstreamTlsConfig result;
    bool generation_seen = false;
    bool verification_seen = false;
    bool ca_pem_seen = false;
    bool server_name_seen = false;
    bool verify_name_seen = false;
    bool client_identity_ref_seen = false;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        bool *seen = nullptr;
        if (entry.key == "generation") {
            seen = &generation_seen;
        } else if (entry.key == "verification") {
            seen = &verification_seen;
        } else if (entry.key == "ca_pem") {
            seen = &ca_pem_seen;
        } else if (entry.key == "server_name") {
            seen = &server_name_seen;
        } else if (entry.key == "verify_name") {
            seen = &verify_name_seen;
        } else if (entry.key == "client_identity_ref") {
            seen = &client_identity_ref_seen;
        } else {
            return invalid_field(path, "unknown upstream_tls field");
        }
        if (*seen) {
            return invalid_field(path, "duplicate upstream_tls field");
        }
        *seen = true;

        if (entry.key == "generation") {
            if (!entry.value.is_integer() || entry.value.as_integer() <= 0) {
                return invalid_field(path, "generation must be a positive integer");
            }
            result.generation = static_cast<std::uint64_t>(entry.value.as_integer());
        } else if (entry.key == "verification") {
            if (!entry.value.is_text()) {
                return invalid_field(path, "verification must be an enum name");
            }
            const std::string_view mode = entry.value.as_text();
            if (mode == "INHERIT") {
                result.verification = UpstreamTlsVerificationMode::Inherit;
            } else if (mode == "LEGACY_INSECURE") {
                result.verification = UpstreamTlsVerificationMode::LegacyInsecure;
            } else if (mode == "SYSTEM_CA") {
                result.verification = UpstreamTlsVerificationMode::SystemCa;
            } else if (mode == "CUSTOM_CA") {
                result.verification = UpstreamTlsVerificationMode::CustomCa;
            } else {
                return invalid_field(path, "unknown upstream TLS verification mode");
            }
        } else {
            std::optional<std::string> *output = entry.key == "ca_pem"        ? &result.ca_pem
                                                 : entry.key == "server_name" ? &result.server_name
                                                 : entry.key == "verify_name" ? &result.verify_name
                                                                              : &result.client_identity_ref;
            if (entry.value.is_null()) {
                output->reset();
            } else if (entry.value.is_text()) {
                output->emplace(entry.value.as_text());
            } else {
                return invalid_field(path, "upstream TLS string field must be a string or null");
            }
        }
    }
    if (!generation_seen) {
        return invalid_field(child_path(field, "generation"), "generation is required");
    }
    return std::optional<RouteUpstreamTlsConfig>(std::move(result));
}

DecodeResult<RouteConfig> route_config(const AccessJsonValue &value, std::string_view field,
                                       const ProjectRouteLimits &limits) {
    if (!value.is_object()) {
        return invalid_field(field, "expected route object");
    }

    RouteConfig result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        if (entry.key == "path") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.path = std::move(*decoded);
        } else if (entry.key == "method") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.method = std::move(*decoded);
        } else if (entry.key == "type") {
            auto decoded = java_enum<RouteType>(
                    entry.value, path,
                    {{"PROXY", RouteType::Proxy}, {"RESPONSE", RouteType::Response}, {"SCRIPT", RouteType::Script}});
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.type = *decoded;
        } else if (entry.key == "service") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.service = std::move(*decoded);
        } else if (entry.key == "cluster") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.cluster = std::move(*decoded);
        } else if (entry.key == "addresses") {
            auto decoded = string_set(entry.value, path, limits.max_addresses_per_route);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.addresses = std::move(*decoded);
        } else if (entry.key == "condition") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.condition = std::move(*decoded);
        } else if (entry.key == "proxy_headers") {
            auto decoded = string_map(entry.value, path, limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.proxy_headers = std::move(*decoded);
        } else if (entry.key == "response_headers") {
            auto decoded = string_map(entry.value, path, limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.response_headers = std::move(*decoded);
        } else if (entry.key == "context") {
            auto decoded = string_map(entry.value, path, limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.context = std::move(*decoded);
        } else if (entry.key == "rewrite") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.rewrite = std::move(*decoded);
        } else if (entry.key == "status") {
            auto decoded = java_int32(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.status = *decoded;
        } else if (entry.key == "body") {
            auto decoded = route_body(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.body = std::move(*decoded);
        } else if (entry.key == "gzip") {
            auto decoded = response_gzip(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.gzip = *decoded;
        } else if (entry.key == "timeout") {
            auto decoded = duration_millis(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.timeout_millis = *decoded;
        } else if (entry.key == "max_client_body_size") {
            auto decoded = data_size(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.max_client_body_size = *decoded;
        } else if (entry.key == "max_proxy_body_size") {
            auto decoded = data_size(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.max_proxy_body_size = *decoded;
        } else if (entry.key == "websocket_timeout") {
            auto decoded = duration_millis(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.websocket_timeout_millis = *decoded;
        } else if (entry.key == "flush") {
            auto decoded = nullable_java_bool(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.flush = *decoded;
        } else if (entry.key == "allows") {
            auto decoded = string_set(entry.value, path, limits.max_cidrs_per_route);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.allows = std::move(*decoded);
        } else if (entry.key == "script") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.script = std::move(*decoded);
        } else if (entry.key == "upstream_tls") {
            auto decoded = route_upstream_tls(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.upstream_tls = std::move(*decoded);
        }
    }
    return result;
}

DecodeResult<std::optional<std::vector<std::optional<RouteConfig>>>>
route_list(const AccessJsonValue &value, std::string_view field, const ProjectRouteLimits &limits) {
    if (value.is_null()) {
        return std::optional<std::vector<std::optional<RouteConfig>>>{};
    }
    if (!value.is_array()) {
        return invalid_field(field, "expected array or null");
    }

    std::vector<std::optional<RouteConfig>> routes;
    const json::JsonArray<AccessJsonValue> &array = value.as_array();
    if (array.size() > limits.max_routes) {
        return limit_exceeded(field, "route count exceeds the configured limit");
    }
    routes.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].is_null()) {
            routes.emplace_back();
            continue;
        }
        auto decoded = route_config(array[i], index_path(field, i), limits);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        routes.emplace_back(std::move(*decoded));
    }
    return std::optional<std::vector<std::optional<RouteConfig>>>(std::move(routes));
}

DecodeResult<std::uint8_t> net_mask(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return invalid_field(field, "Java HostStrategy.setNet rejects null");
    }
    auto decoded = java_string(value, field);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }

    std::uint8_t result = 0;
    std::string_view text = *decoded;
    std::size_t offset = 0;
    while (true) {
        const std::size_t separator = text.find(',', offset);
        const std::string_view item =
                separator == std::string_view::npos ? text.substr(offset) : text.substr(offset, separator - offset);
        if (item == "S_VDI") {
            result |= kNetVdi;
        } else if (item == "S_OFFICE") {
            result |= kNetOffice;
        } else if (item == "S_INTERNET") {
            result |= kNetInternet;
        } else if (item == "S_CUSTOM") {
            result |= kNetCustom;
        } else {
            return invalid_field(field, "unknown HostStrategy net enum");
        }
        if (separator == std::string_view::npos) {
            return result;
        }
        offset = separator + 1;
    }
}

DecodeResult<HostStrategyConfig> host_strategy(const AccessJsonValue &value, std::string_view field) {
    if (!value.is_object()) {
        return invalid_field(field, "expected HostStrategy object");
    }

    HostStrategyConfig result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        if (entry.key == "https") {
            auto decoded = java_enum<HttpsStrategy>(entry.value, path,
                                                    {{"S_NOT_MUST", HttpsStrategy::NotRequired},
                                                     {"S_301", HttpsStrategy::Redirect301},
                                                     {"S_302", HttpsStrategy::Redirect302},
                                                     {"S_307", HttpsStrategy::Redirect307},
                                                     {"S_308", HttpsStrategy::Redirect308}});
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.https = *decoded;
        } else if (entry.key == "net") {
            auto decoded = net_mask(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.net_mask = *decoded;
        }
    }
    return result;
}

void set_host_entry(std::vector<HostConfigEntry> &entries, std::string pattern,
                    std::optional<HostStrategyConfig> strategy) {
    for (HostConfigEntry &entry: entries) {
        if (entry.pattern == pattern) {
            entry.strategy = std::move(strategy);
            return;
        }
    }
    entries.push_back(HostConfigEntry{.pattern = std::move(pattern), .strategy = std::move(strategy)});
}

DecodeResult<std::optional<std::vector<HostConfigEntry>>> host_map(const AccessJsonValue &value, std::string_view field,
                                                                   const ProjectRouteLimits &limits) {
    if (value.is_null()) {
        return std::optional<std::vector<HostConfigEntry>>{};
    }
    if (!value.is_object()) {
        return invalid_field(field, "expected object or null");
    }
    if (value.as_object().size() > limits.max_hosts) {
        return limit_exceeded(field, "host count exceeds the configured limit");
    }

    std::vector<HostConfigEntry> hosts;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        std::optional<HostStrategyConfig> strategy;
        if (!entry.value.is_null()) {
            auto decoded = host_strategy(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            strategy.emplace(std::move(*decoded));
        }
        set_host_entry(hosts, std::string(entry.key), std::move(strategy));
    }
    return std::optional<std::vector<HostConfigEntry>>(std::move(hosts));
}

DecodeResult<ProjectConfig> project_config(const json::JsonObject<AccessJsonValue> &object,
                                           const ProjectRouteLimits &limits) {
    ProjectConfig result;
    for (const auto &entry: object) {
        if (entry.key == "version") {
            auto decoded = java_int32(entry.value, "version");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.version = *decoded;
        } else if (entry.key == "host") {
            auto decoded = host_map(entry.value, "host", limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.hosts = std::move(*decoded);
        } else if (entry.key == "routes") {
            auto decoded = route_list(entry.value, "routes", limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.routes = std::move(*decoded);
        }
    }
    return result;
}

} // namespace

ProjectConfigResult parse_project_config(std::string_view content, const AccessConfigLimits &limits) {
    if (content.empty()) {
        return std::optional<ProjectConfig>{};
    }
    if (content.size() > limits.project_route.max_payload_bytes) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::LimitExceeded, "payload",
                                                         "project route payload exceeds the configured byte limit"));
    }

    mem::BufPool pool;
    json::JsonParser parser;
    auto root = config_detail::parse_access_json(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    if (root->is_null()) {
        return std::optional<ProjectConfig>{};
    }
    if (!root->is_object()) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::InvalidRoot, {},
                                                         "project configuration must be an object or null"));
    }

    auto decoded = project_config(root->as_object(), limits.project_route);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    auto within_limits = validate_project_config_limits(*decoded, limits);
    if (!within_limits) {
        return std::unexpected(std::move(within_limits.error()));
    }
    return std::optional<ProjectConfig>(std::move(*decoded));
}

} // namespace fiber::access_server
