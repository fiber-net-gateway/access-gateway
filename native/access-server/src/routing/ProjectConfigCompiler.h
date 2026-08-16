#ifndef FIBER_ACCESS_SERVER_PROJECT_CONFIG_COMPILER_H
#define FIBER_ACCESS_SERVER_PROJECT_CONFIG_COMPILER_H

#include "../config/AccessConfigLimits.h"
#include "ProjectRouteSnapshot.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <fiber/http_script/ConstPackage.h>

namespace fiber::access_server {

struct ScriptCompilerAdapter {
    using Result = std::expected<script::Script, std::string>;
    using Function = Result (*)(void *context, http_script::ConstPackage::Builder &constants,
                                std::string_view expression, std::span<const std::string> path_variable_names);
    using RouteFunction = Result (*)(void *context, http_script::ConstPackage::Builder &constants,
                                     std::string_view source, std::span<const std::string> path_variable_names);

    void *context = nullptr;
    Function compile_expression = nullptr;
    RouteFunction compile_route_script = nullptr;
};

using ProjectSnapshotResult = std::expected<std::optional<ProjectRouteSnapshot>, AccessConfigError>;

// Pure, synchronous ProjectConfig -> ProjectRouteSnapshot compiler. It owns no
// runtime publication or Nacos state and may run on the dedicated compiler loop.
class ProjectConfigCompiler final {
public:
    explicit ProjectConfigCompiler(ScriptCompilerAdapter script_compiler = {},
                                   const AccessConfigLimits &limits = kAccessConfigLimits) noexcept :
        script_compiler_(script_compiler), limits_(&limits) {}

    // A missing/empty host map is the Java watcher unload signal and returns
    // std::nullopt without compiling routes.
    [[nodiscard]] ProjectSnapshotResult compile(std::string_view project, const ProjectConfig &config) const;

private:
    ScriptCompilerAdapter script_compiler_;
    const AccessConfigLimits *limits_ = nullptr;
};

// Compatibility entry points for validators, benchmarks, and focused tests.
[[nodiscard]] ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config);
[[nodiscard]] ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config,
                                                           ScriptCompilerAdapter compiler);
[[nodiscard]] ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config,
                                                           ScriptCompilerAdapter compiler,
                                                           ProxyAddressSelectorFactory selector_factory,
                                                           const AccessConfigLimits &limits = kAccessConfigLimits);

// Pure compilation leaves service metadata in unavailable selectors. Runtime
// callers replace them with owner-loop NamingService leases before readiness.
[[nodiscard]] std::expected<void, AccessConfigError>
bind_project_service_selectors(ProjectRouteSnapshot &snapshot, ProxyAddressSelectorFactory selector_factory);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROJECT_CONFIG_COMPILER_H
