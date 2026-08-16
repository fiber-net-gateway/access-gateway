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
using ProjectListResult = std::expected<std::vector<std::string>, AccessConfigError>;

// Empty content and a JSON null match AccessRouteConfigWatcher's empty-conf
// path and produce std::nullopt.
[[nodiscard]] ProjectConfigResult parse_project_config(std::string_view content,
                                                       const AccessConfigLimits &limits = kAccessConfigLimits);

// Empty wire content is the Java interceptor's keep-current signal. A JSON
// null or empty object is an accepted update that clears all gray rules.
[[nodiscard]] GrayMatchConfigResult parse_gray_match_config(std::string_view content,
                                                            const AccessConfigLimits &limits = kAccessConfigLimits);

// Matches DynamicRouteConfigWatcher: the complete value is trimmed once and
// then split on ';'. Individual project names are not trimmed.
[[nodiscard]] ProjectListResult parse_project_list(std::string_view content,
                                                   const AccessConfigLimits &limits = kAccessConfigLimits);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_CODEC_H
