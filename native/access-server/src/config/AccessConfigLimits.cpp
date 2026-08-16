#include "AccessConfigLimits.h"

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace fiber::access_server {
namespace {

AccessConfigError limit_error(std::string field, std::string_view resource, std::size_t maximum) {
    std::array<char, 32> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), maximum);
    std::string message(resource);
    message.append(" exceeds the limit of ");
    message.append(digits.data(), converted.ptr);
    return AccessConfigError{
            .code = AccessConfigErrorCode::LimitExceeded,
            .field = std::move(field),
            .message = std::move(message),
    };
}

std::string route_field(std::size_t route_index, std::string_view field) {
    std::string path = "routes[";
    path.append(std::to_string(route_index));
    path.push_back(']');
    if (!field.empty()) {
        path.push_back('.');
        path.append(field);
    }
    return path;
}

std::string indexed_field(std::string_view parent, std::size_t index, std::string_view child = {}) {
    std::string field(parent);
    field.push_back('[');
    field.append(std::to_string(index));
    field.push_back(']');
    if (!child.empty()) {
        field.push_back('.');
        field.append(child);
    }
    return field;
}

std::optional<AccessConfigError> check_optional_string(const std::optional<std::string> &value, std::string field,
                                                       std::string_view resource, std::size_t maximum) {
    if (value && value->size() > maximum) {
        return limit_error(std::move(field), resource, maximum);
    }
    return std::nullopt;
}

std::optional<AccessConfigError> check_string_map(const StringConfigMap &entries, std::string_view field,
                                                  const ProjectRouteLimits &limits) {
    if (entries.size() > limits.max_header_entries) {
        return limit_error(std::string(field), "entry count", limits.max_header_entries);
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const StringConfigEntry &entry = entries[index];
        if (entry.name.size() > limits.max_header_name_bytes) {
            return limit_error(indexed_field(field, index, "name"), "name bytes", limits.max_header_name_bytes);
        }
        if (entry.value && entry.value->size() > limits.max_header_value_bytes) {
            return limit_error(indexed_field(field, index, "value"), "value bytes", limits.max_header_value_bytes);
        }
    }
    return std::nullopt;
}

std::optional<AccessConfigError> check_string_set(const NullableStringSet &values, std::string_view field,
                                                  std::size_t max_items, std::size_t max_bytes,
                                                  std::string_view resource) {
    if (values.size() > max_items) {
        return limit_error(std::string(field), "entry count", max_items);
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index] && values[index]->size() > max_bytes) {
            return limit_error(indexed_field(field, index), resource, max_bytes);
        }
    }
    return std::nullopt;
}

std::size_t max_base64_body_bytes(std::size_t decoded_bytes) noexcept { return ((decoded_bytes + 2U) / 3U) * 4U; }

std::optional<AccessConfigError> validate_route(const RouteConfig &route, std::size_t route_index,
                                                const ProjectRouteLimits &limits) {
    const auto check = [&](const std::optional<std::string> &value, std::string_view field, std::string_view resource,
                           std::size_t maximum) {
        return check_optional_string(value, route_field(route_index, field), resource, maximum);
    };
    if (auto error = check(route.path, "path", "path bytes", limits.max_path_bytes)) {
        return error;
    }
    if (auto error = check(route.method, "method", "method bytes", limits.max_method_bytes)) {
        return error;
    }
    if (auto error = check(route.service, "service", "service bytes", limits.max_service_bytes)) {
        return error;
    }
    if (route.service) {
        const std::size_t slash = route.service->find('/');
        if (slash > 0 && slash + 1 < route.service->size() &&
            route.service->size() - slash - 1 > limits.max_cluster_bytes) {
            return limit_error(route_field(route_index, "service"), "service cluster bytes", limits.max_cluster_bytes);
        }
    }
    if (auto error = check(route.cluster, "cluster", "cluster bytes", limits.max_cluster_bytes)) {
        return error;
    }
    if (auto error = check(route.condition, "condition", "condition bytes", limits.max_condition_bytes)) {
        return error;
    }
    if (auto error = check(route.rewrite, "rewrite", "template bytes", limits.max_template_bytes)) {
        return error;
    }
    if (auto error = check(route.script, "script", "script bytes", limits.max_script_bytes)) {
        return error;
    }
    if (auto error = check_string_set(route.addresses, route_field(route_index, "addresses"),
                                      limits.max_addresses_per_route, limits.max_address_bytes, "address bytes")) {
        return error;
    }
    if (auto error = check_string_set(route.allows, route_field(route_index, "allows"), limits.max_cidrs_per_route,
                                      limits.max_cidr_bytes + 1U, "CIDR bytes")) {
        return error;
    }
    for (const auto &[map, field]:
         {std::pair<const StringConfigMap *, std::string_view>{&route.proxy_headers, "proxy_headers"},
          {&route.response_headers, "response_headers"},
          {&route.context, "context"}}) {
        if (auto error = check_string_map(*map, route_field(route_index, field), limits)) {
            return error;
        }
    }
    if (route.body && route.body->content) {
        std::size_t maximum = limits.max_template_bytes;
        std::string_view resource = "template bytes";
        if (route.body->type == BodyType::Text) {
            maximum = limits.max_static_response_body_bytes;
            resource = "response body bytes";
        } else if (route.body->type == BodyType::Base64) {
            maximum = max_base64_body_bytes(limits.max_static_response_body_bytes);
            resource = "encoded response body bytes";
        }
        if (route.body->content->size() > maximum) {
            return limit_error(route_field(route_index, "body.content"), resource, maximum);
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<void, AccessConfigError> validate_project_name_limit(std::string_view project,
                                                                   const AccessConfigLimits &limits) {
    if (project.size() > limits.project_list.max_project_name_bytes) {
        return std::unexpected(
                limit_error("project", "project name bytes", limits.project_list.max_project_name_bytes));
    }
    return {};
}

std::expected<void, AccessConfigError> validate_project_config_limits(const ProjectConfig &config,
                                                                      const AccessConfigLimits &limits) {
    const ProjectRouteLimits &route_limits = limits.project_route;
    if (config.hosts && config.hosts->size() > route_limits.max_hosts) {
        return std::unexpected(limit_error("host", "host count", route_limits.max_hosts));
    }
    if (config.hosts) {
        for (std::size_t index = 0; index < config.hosts->size(); ++index) {
            if ((*config.hosts)[index].pattern.size() > route_limits.max_host_pattern_bytes) {
                return std::unexpected(limit_error(indexed_field("host", index, "pattern"), "host pattern bytes",
                                                   route_limits.max_host_pattern_bytes));
            }
        }
    }
    if (config.routes && config.routes->size() > route_limits.max_routes) {
        return std::unexpected(limit_error("routes", "route count", route_limits.max_routes));
    }
    if (config.routes) {
        for (std::size_t index = 0; index < config.routes->size(); ++index) {
            if (!(*config.routes)[index]) {
                continue;
            }
            if (auto error = validate_route(*(*config.routes)[index], index, route_limits)) {
                return std::unexpected(std::move(*error));
            }
        }
    }
    return {};
}

std::expected<void, AccessConfigError> validate_gray_match_config_limits(const GrayMatchConfig &config,
                                                                         const AccessConfigLimits &limits) {
    const GrayRuleLimits &gray_limits = limits.gray_rules;
    if (config.size() > gray_limits.max_rules) {
        return std::unexpected(limit_error("rules", "gray rule count", gray_limits.max_rules));
    }
    for (std::size_t index = 0; index < config.size(); ++index) {
        const GrayMatchConfigEntry &entry = config[index];
        if (entry.entry.size() > gray_limits.max_entry_bytes) {
            return std::unexpected(limit_error(indexed_field("rules", index, "entry"), "gray entry bytes",
                                               gray_limits.max_entry_bytes));
        }
        if (auto error = check_string_set(entry.cidrs, indexed_field("rules", index, "cidrs"),
                                          gray_limits.max_cidrs_per_rule, gray_limits.max_cidr_bytes, "CIDR bytes")) {
            return std::unexpected(std::move(*error));
        }
    }
    return {};
}

} // namespace fiber::access_server
