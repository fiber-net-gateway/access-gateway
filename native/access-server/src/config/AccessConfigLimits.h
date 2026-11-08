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

struct EdgeFunctionLimits {
    // Index document (ploto.unified-access.edge-functions) byte budget.
    std::size_t max_payload_bytes = 256U << 10U;
    std::size_t max_functions = 64;
    // Slug identity cap (the name also forms the script Data ID).
    std::size_t max_function_name_bytes = 64;
    std::size_t max_globs_per_selector = 256;
    std::size_t max_glob_bytes = 255;
    // Per-function script Data ID payload budget.
    std::size_t max_script_payload_bytes = 1U << 20U;
    // Sum across all functions in one snapshot.
    std::size_t max_total_script_bytes = 8U << 20U;
    // Per function per request; excess reports are dropped and counted.
    std::size_t max_reports_per_execution = 16;
    // cidr directive budgets for the restricted edge-function library.
    std::size_t max_cidr_directives_per_script = 16;
    std::size_t max_cidrs_per_directive = 256;
    std::size_t max_cidr_bytes = 64;
    // Truncation applied to request fields exposed to edge functions.
    std::size_t max_input_field_bytes = 8U << 10U;
    // Request-body phase budget (design §3.2): bodies at or above this size
    // never enter the body-function path (streamed untouched); buffered
    // prefixes beyond it are spliced through without executing functions.
    std::size_t max_body_phase_bytes = 1U << 20U;
};

struct AccessConfigLimits {
    // Version 3 added EdgeFunctionLimits; version 4 collapses the
    // edge-function identity to a single slug name (maxFunctionIdBytes
    // removed, maxFunctionNameBytes is now the slug cap); version 5 adds the
    // request-body phase budget (maxBodyPhaseBytes); version 6 flattens the
    // single mount onto the function (maxMountsPerFunction removed).
    std::size_t schema_version = 6;
    ProjectListLimits project_list;
    ProjectRouteLimits project_route;
    GrayRuleLimits gray_rules;
    EdgeFunctionLimits edge_functions;
};

inline constexpr AccessConfigLimits kAccessConfigLimits{};

[[nodiscard]] std::expected<void, AccessConfigError>
validate_project_name_limit(std::string_view project, const AccessConfigLimits &limits = kAccessConfigLimits);

[[nodiscard]] std::expected<void, AccessConfigError>
validate_project_config_limits(const ProjectConfig &config, const AccessConfigLimits &limits = kAccessConfigLimits);

[[nodiscard]] std::expected<void, AccessConfigError>
validate_gray_match_config_limits(const GrayMatchConfig &config,
                                  const AccessConfigLimits &limits = kAccessConfigLimits);

[[nodiscard]] std::expected<void, AccessConfigError>
validate_edge_function_index_limits(const EdgeFunctionIndexConfig &config,
                                    const AccessConfigLimits &limits = kAccessConfigLimits);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_LIMITS_H
