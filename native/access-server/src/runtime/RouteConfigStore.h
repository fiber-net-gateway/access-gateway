#ifndef FIBER_ACCESS_SERVER_ROUTE_CONFIG_STORE_H
#define FIBER_ACCESS_SERVER_ROUTE_CONFIG_STORE_H

#include "../routing/ProjectConfigCompiler.h"
#include "AccessServiceDiscovery.h"
#include "ProjectSnapshotRegistry.h"
#include "RouteSnapshotPublisher.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Task.h>

namespace fiber::event {

class EventLoopGroup;

} // namespace fiber::event

namespace fiber::access_server {

enum class ConfigUpdateStatus : std::uint8_t {
    IgnoredEmpty,
    VersionUnchanged,
    Published,
    Unloaded,
    ProjectRemoved,
};

struct ConfigUpdateResult {
    ConfigUpdateStatus status = ConfigUpdateStatus::IgnoredEmpty;
    std::shared_ptr<const AccessRouteSnapshot> snapshot;
    std::chrono::nanoseconds global_build_duration{};
    std::chrono::nanoseconds publish_duration{};
};

using ConfigUpdateOutcome = std::expected<ConfigUpdateResult, AccessConfigError>;

struct ConfigBatchProjectResult {
    std::string project;
    std::expected<ConfigUpdateStatus, AccessConfigError> outcome;
};

struct ConfigBatchUpdateResult {
    std::vector<ConfigBatchProjectResult> projects;
    std::shared_ptr<const AccessRouteSnapshot> snapshot;
    std::chrono::nanoseconds global_build_duration{};
    std::chrono::nanoseconds publish_duration{};
    bool published = false;
};

using ConfigBatchUpdateOutcome = std::expected<ConfigBatchUpdateResult, AccessConfigError>;

class PreparedProjectUpdate;

// Proof that a prepared Project candidate completed every required readiness
// transition. Only PreparedProjectUpdate can create this move-only type.
class ReadyProjectUpdate final {
public:
    ReadyProjectUpdate(const ReadyProjectUpdate &) = delete;
    ReadyProjectUpdate &operator=(const ReadyProjectUpdate &) = delete;
    ReadyProjectUpdate(ReadyProjectUpdate &&other) noexcept;
    ReadyProjectUpdate &operator=(ReadyProjectUpdate &&other) noexcept;
    ~ReadyProjectUpdate() = default;

private:
    friend class PreparedProjectUpdate;
    friend class RouteConfigStore;

    ReadyProjectUpdate(ConfigUpdateStatus status, std::string project, std::int32_t version,
                       std::shared_ptr<const ProjectRouteSnapshot> project_snapshot) noexcept :
        status_(status), project_(std::move(project)), version_(version),
        project_snapshot_(std::move(project_snapshot)) {}

    ConfigUpdateStatus status_ = ConfigUpdateStatus::IgnoredEmpty;
    std::string project_;
    std::int32_t version_ = 0;
    std::shared_ptr<const ProjectRouteSnapshot> project_snapshot_;
    bool valid_ = true;
};

using ReadyProjectUpdateOutcome = std::expected<ReadyProjectUpdate, ProxyAddressReadyError>;

// Owns a fully compiled and selector-bound Project candidate. It cannot be
// committed until one of its checked one-way transitions produces ReadyProjectUpdate.
class PreparedProjectUpdate final {
public:
    PreparedProjectUpdate(const PreparedProjectUpdate &) = delete;
    PreparedProjectUpdate &operator=(const PreparedProjectUpdate &) = delete;
    PreparedProjectUpdate(PreparedProjectUpdate &&other) noexcept;
    PreparedProjectUpdate &operator=(PreparedProjectUpdate &&other) noexcept;
    ~PreparedProjectUpdate() = default;

    [[nodiscard]] std::optional<ReadyProjectUpdate> try_ready() && noexcept;
    [[nodiscard]] async::Task<ReadyProjectUpdateOutcome> wait_ready() && noexcept;

private:
    friend class RouteConfigStore;

    PreparedProjectUpdate(ConfigUpdateStatus status, std::string project, std::int32_t version,
                          std::shared_ptr<const ProjectRouteSnapshot> project_snapshot) noexcept :
        status_(status), project_(std::move(project)), version_(version),
        project_snapshot_(std::move(project_snapshot)) {}

    [[nodiscard]] ReadyProjectUpdate into_ready() && noexcept;
    [[nodiscard]] static async::Task<ReadyProjectUpdateOutcome>
    wait_ready_impl(PreparedProjectUpdate prepared) noexcept;

    ConfigUpdateStatus status_ = ConfigUpdateStatus::IgnoredEmpty;
    std::string project_;
    std::int32_t version_ = 0;
    std::shared_ptr<const ProjectRouteSnapshot> project_snapshot_;
    bool valid_ = true;
};

using PreparedProjectUpdateOutcome = std::expected<PreparedProjectUpdate, AccessConfigError>;

// Mutation is serialized by the runtime owner EventLoop. Requests may pin the
// immutable published snapshot from any serving thread.
class RouteConfigStore {
public:
    explicit RouteConfigStore(ScriptCompilerAdapter script_compiler = {},
                              ProxyAddressSelectorFactory selector_factory = {});
    RouteConfigStore(event::EventLoopGroup &workers, ScriptCompilerAdapter script_compiler = {},
                     ProxyAddressSelectorFactory selector_factory = {});
    RouteConfigStore(ScriptCompilerAdapter script_compiler, AccessServiceDiscovery &service_discovery,
                     AccessServiceDiscoveryOptions discovery_options = {},
                     AccessDiscoveryMetricsObserver metrics_observer = {});
    RouteConfigStore(event::EventLoopGroup &workers, ScriptCompilerAdapter script_compiler,
                     AccessServiceDiscovery &service_discovery, AccessServiceDiscoveryOptions discovery_options = {},
                     AccessDiscoveryMetricsObserver metrics_observer = {});

    [[nodiscard]] PreparedProjectUpdateOutcome prepare(std::string_view project,
                                                       const std::optional<ProjectConfig> &config);
    [[nodiscard]] PreparedProjectUpdateOutcome prepare_compiled(std::string_view project,
                                                                std::optional<std::int32_t> version,
                                                                std::optional<ProjectRouteSnapshot> project_snapshot);
    [[nodiscard]] ConfigUpdateOutcome apply(std::string_view project, const std::optional<ProjectConfig> &config);
    [[nodiscard]] ConfigUpdateOutcome commit(ReadyProjectUpdate ready);
    // Applies one deterministic project-name-ordered transaction. A candidate
    // rejected by cross-project validation retains that project's old record;
    // accepted siblings are assembled and atomically published once.
    [[nodiscard]] ConfigBatchUpdateOutcome commit_batch(std::vector<ReadyProjectUpdate> ready);
    [[nodiscard]] ConfigUpdateOutcome remove_project(std::string_view project);
    void clear() noexcept;

    [[nodiscard]] std::optional<std::int32_t> current_version(std::string_view project) const noexcept;
    [[nodiscard]] AccessRouteSnapshotProvider snapshot_provider() const noexcept;

    [[nodiscard]] std::shared_ptr<const AccessRouteSnapshot> pin() const noexcept { return publisher_.pin(); }

private:
    static void apply_to_candidate(std::vector<std::shared_ptr<const ProjectRouteSnapshot>> &candidate,
                                   const ReadyProjectUpdate &ready);

    ProjectConfigCompiler project_compiler_;
    AccessServiceSelectorFactory service_selector_factory_;
    ProxyAddressSelectorFactory selector_factory_;
    ProjectSnapshotRegistry registry_;
    RouteSnapshotPublisher publisher_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ROUTE_CONFIG_STORE_H
