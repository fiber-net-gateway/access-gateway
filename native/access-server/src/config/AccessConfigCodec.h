#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_CODEC_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_CODEC_H

#include "AccessConfig.h"
#include "AccessConfigError.h"
#include "AccessConfigLimits.h"

#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace fiber::access_server {

using ProjectConfigResult = std::expected<std::optional<ProjectConfig>, AccessConfigError>;
using GrayMatchConfigResult = std::expected<std::optional<GrayMatchConfig>, AccessConfigError>;
using TempClusterConfigResult = std::expected<std::optional<TempClusterConfig>, AccessConfigError>;
using EdgeFunctionIndexResult = std::expected<std::optional<EdgeFunctionIndexConfig>, AccessConfigError>;
using ProjectListResult = std::expected<std::vector<std::string>, AccessConfigError>;

// Empty content and a JSON null match AccessRouteConfigWatcher's empty-conf
// path and produce std::nullopt.
[[nodiscard]] ProjectConfigResult parse_project_config(std::string_view content,
                                                       const AccessConfigLimits &limits = kAccessConfigLimits);

// Empty wire content is the Java interceptor's keep-current signal. A JSON
// null or empty object is an accepted update that clears all gray rules.
[[nodiscard]] GrayMatchConfigResult parse_gray_match_config(std::string_view content,
                                                            const AccessConfigLimits &limits = kAccessConfigLimits);

// Java temp-cluster whitelist (test environments only): every valid payload —
// including empty and null content — is an accepted update; an empty payload
// clears the whitelist. Invalid payloads are rejected so the watcher can keep
// the previous snapshot.
[[nodiscard]] TempClusterConfigResult parse_temp_cluster_config(std::string_view content);

// Edge-function index. This is the project's own wire format (no Java
// compatibility), so it is strict: unknown fields are rejected. Empty content
// is the watcher's keep-current signal; a JSON null or an explicit empty
// functions list is an accepted update that unmounts every function.
[[nodiscard]] EdgeFunctionIndexResult
parse_edge_function_index_config(std::string_view content, const AccessConfigLimits &limits = kAccessConfigLimits);

// Matches DynamicRouteConfigWatcher: the complete value is trimmed once and
// then split on ';'. Individual project names are not trimmed.
[[nodiscard]] ProjectListResult parse_project_list(std::string_view content,
                                                   const AccessConfigLimits &limits = kAccessConfigLimits);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_CODEC_H
