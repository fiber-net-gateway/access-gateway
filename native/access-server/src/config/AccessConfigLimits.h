#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_LIMITS_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_LIMITS_H

#include "AccessConfig.h"
#include "AccessConfigError.h"

#include <cstddef>
#include <expected>
#include <string_view>

namespace fiber::access_server {

struct ProjectListLimits {
    std::size_t max_payload_bytes = 256U << 10U;
    std::size_t max_projects = 1024;
    std::size_t max_project_name_bytes = 255;
};

struct ProjectRouteLimits {
    std::size_t max_payload_bytes = 4U << 20U;
    std::size_t max_hosts = 1024;
    std::size_t max_routes = 5000;
    std::size_t max_host_pattern_bytes = 255;
    std::size_t max_path_bytes = 2048;
    std::size_t max_method_bytes = 64;
    std::size_t max_service_bytes = 1024;
    std::size_t max_cluster_bytes = 255;
    std::size_t max_condition_bytes = 256U << 10U;
    std::size_t max_script_bytes = 1U << 20U;
    std::size_t max_template_bytes = 1U << 20U;
    std::size_t max_header_entries = 256;
    std::size_t max_header_name_bytes = 256;
    std::size_t max_header_value_bytes = 64U << 10U;
    std::size_t max_cidrs_per_route = 256;
    std::size_t max_cidr_bytes = 64;
    std::size_t max_addresses_per_route = 256;
    std::size_t max_address_bytes = 2048;
    std::size_t max_upstream_tls_profiles = 256;
    std::size_t max_upstream_tls_ca_pem_bytes = 512U << 10U;
    std::size_t max_static_response_body_bytes = 2U << 20U;
    std::size_t max_static_response_bytes = 8U << 20U;
    std::size_t max_path_variables = 64;
    std::size_t max_template_expressions = 256;
    std::size_t max_compiled_programs = 20000;
    std::size_t max_estimated_snapshot_bytes = 64U << 20U;
};

struct GrayRuleLimits {
    std::size_t max_payload_bytes = 256U << 10U;
    std::size_t max_rules = 16;
    std::size_t max_entry_bytes = 64;
    std::size_t max_cidrs_per_rule = 256;
    std::size_t max_cidr_bytes = 64;
};

struct AccessConfigLimits {
    std::size_t schema_version = 2;
    ProjectListLimits project_list;
    ProjectRouteLimits project_route;
    GrayRuleLimits gray_rules;
};

inline constexpr AccessConfigLimits kAccessConfigLimits{};

[[nodiscard]] std::expected<void, AccessConfigError>
validate_project_name_limit(std::string_view project, const AccessConfigLimits &limits = kAccessConfigLimits);

[[nodiscard]] std::expected<void, AccessConfigError>
validate_project_config_limits(const ProjectConfig &config, const AccessConfigLimits &limits = kAccessConfigLimits);

[[nodiscard]] std::expected<void, AccessConfigError>
validate_gray_match_config_limits(const GrayMatchConfig &config,
                                  const AccessConfigLimits &limits = kAccessConfigLimits);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_LIMITS_H
