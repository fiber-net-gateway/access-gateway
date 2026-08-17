#include "RouteConfigStore.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <numeric>
#include <unordered_set>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoopGroup.h>

namespace fiber::access_server {
namespace {

using SteadyClock = std::chrono::steady_clock;

std::expected<AccessRouteSnapshot, AccessConfigError>
build_candidate(std::span<const std::shared_ptr<const ProjectRouteSnapshot>> candidate,
                std::chrono::nanoseconds &elapsed) {
    const SteadyClock::time_point started = SteadyClock::now();
    auto snapshot = AccessRouteSnapshot::build(candidate);
    elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - started);
    return snapshot;
}

AccessConfigError duplicate_batch_project_error() {
    return AccessConfigError{
            .code = AccessConfigErrorCode::Conflict,
            .field = "projects",
            .message = "batch contains duplicate project updates",
    };
}

struct HostPatternIdentityHash {
    [[nodiscard]] std::size_t operator()(std::string_view pattern) const noexcept {
        return host_pattern_identity_hash(pattern);
    }
};

struct HostPatternIdentityEqual {
    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept {
        return host_pattern_identity_equals(left, right);
    }
};

AccessConfigError duplicate_host_error(std::string_view pattern) {
    return AccessConfigError{
            .code = AccessConfigErrorCode::InvalidField,
            .field = "host",
            .message = pattern.starts_with('*') ? "wildcard is duplicate" : "host is duplicate",
    };
}

AccessConfigError project_count_error() {
    return AccessConfigError{
            .code = AccessConfigErrorCode::LimitExceeded,
            .field = "projects",
            .message = "project count exceeds the configured limit",
    };
}

} // namespace

ReadyProjectUpdate::ReadyProjectUpdate(ReadyProjectUpdate &&other) noexcept :
    status_(other.status_), project_(std::move(other.project_)), version_(other.version_),
    project_snapshot_(std::move(other.project_snapshot_)), valid_(std::exchange(other.valid_, false)) {}

ReadyProjectUpdate &ReadyProjectUpdate::operator=(ReadyProjectUpdate &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    status_ = other.status_;
    project_ = std::move(other.project_);
    version_ = other.version_;
    project_snapshot_ = std::move(other.project_snapshot_);
    valid_ = std::exchange(other.valid_, false);
    return *this;
}

PreparedProjectUpdate::PreparedProjectUpdate(PreparedProjectUpdate &&other) noexcept :
    status_(other.status_), project_(std::move(other.project_)), version_(other.version_),
    project_snapshot_(std::move(other.project_snapshot_)), valid_(std::exchange(other.valid_, false)) {}

PreparedProjectUpdate &PreparedProjectUpdate::operator=(PreparedProjectUpdate &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    status_ = other.status_;
    project_ = std::move(other.project_);
    version_ = other.version_;
    project_snapshot_ = std::move(other.project_snapshot_);
    valid_ = std::exchange(other.valid_, false);
    return *this;
}

ReadyProjectUpdate PreparedProjectUpdate::into_ready() && noexcept {
    FIBER_ASSERT(valid_);
    valid_ = false;
    return ReadyProjectUpdate(status_, std::move(project_), version_, std::move(project_snapshot_));
}

std::optional<ReadyProjectUpdate> PreparedProjectUpdate::try_ready() && noexcept {
    FIBER_ASSERT(valid_);
    if (project_snapshot_ && !project_snapshot_->ready_for_publish()) {
        return std::nullopt;
    }
    return std::move(*this).into_ready();
}

async::Task<ReadyProjectUpdateOutcome> PreparedProjectUpdate::wait_ready() && noexcept {
    FIBER_ASSERT(valid_);
    return wait_ready_impl(std::move(*this));
}

async::Task<ReadyProjectUpdateOutcome> PreparedProjectUpdate::wait_ready_impl(PreparedProjectUpdate prepared) noexcept {
    FIBER_ASSERT(prepared.valid_);
    if (prepared.project_snapshot_) {
        auto ready = co_await prepared.project_snapshot_->wait_ready();
        if (!ready) {
            co_return ReadyProjectUpdateOutcome(std::unexpected(ready.error()));
        }
    }
    co_return ReadyProjectUpdateOutcome(std::in_place, std::move(prepared).into_ready());
}

RouteConfigStore::RouteConfigStore(ScriptCompilerAdapter script_compiler,
                                   ProxyAddressSelectorFactory selector_factory) :
    project_compiler_(script_compiler), selector_factory_(selector_factory) {}

RouteConfigStore::RouteConfigStore(event::EventLoopGroup &workers, ScriptCompilerAdapter script_compiler,
                                   ProxyAddressSelectorFactory selector_factory) :
    project_compiler_(script_compiler), selector_factory_(selector_factory), publisher_(workers) {}

RouteConfigStore::RouteConfigStore(ScriptCompilerAdapter script_compiler, AccessServiceDiscovery &service_discovery,
                                   AccessServiceDiscoveryOptions discovery_options,
                                   AccessDiscoveryMetricsObserver metrics_observer) :
    project_compiler_(script_compiler),
    service_selector_factory_(service_discovery, std::move(discovery_options), metrics_observer),
    selector_factory_(service_selector_factory_.adapter()) {}

RouteConfigStore::RouteConfigStore(event::EventLoopGroup &workers, ScriptCompilerAdapter script_compiler,
                                   AccessServiceDiscovery &service_discovery,
                                   AccessServiceDiscoveryOptions discovery_options,
                                   AccessDiscoveryMetricsObserver metrics_observer) :
    project_compiler_(script_compiler),
    service_selector_factory_(service_discovery, std::move(discovery_options), metrics_observer),
    selector_factory_(service_selector_factory_.adapter()), publisher_(workers) {}

PreparedProjectUpdateOutcome RouteConfigStore::prepare(std::string_view project,
                                                       const std::optional<ProjectConfig> &config) {
    if (!config) {
        return prepare_compiled(project, std::nullopt, std::nullopt);
    }

    const std::optional<std::int32_t> current_version = this->current_version(project);
    if (current_version && *current_version == config->version) {
        return PreparedProjectUpdate(ConfigUpdateStatus::VersionUnchanged, std::string(project), config->version, {});
    }

    auto compiled = project_compiler_.compile(project, *config);
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    return prepare_compiled(project, config->version, std::move(*compiled));
}

PreparedProjectUpdateOutcome RouteConfigStore::prepare_compiled(std::string_view project,
                                                                std::optional<std::int32_t> version,
                                                                std::optional<ProjectRouteSnapshot> project_snapshot) {
    if (!version) {
        return PreparedProjectUpdate(ConfigUpdateStatus::IgnoredEmpty, std::string(project), 0, {});
    }

    const std::optional<std::int32_t> published = current_version(project);
    if (published && *published == *version) {
        return PreparedProjectUpdate(ConfigUpdateStatus::VersionUnchanged, std::string(project), *version, {});
    }

    if (project_snapshot && project_snapshot->project() != project) {
        return std::unexpected(AccessConfigError{
                .code = AccessConfigErrorCode::InvalidCombination,
                .field = "project",
                .message = "compiled snapshot project does not match the prepared update",
        });
    }
    if (project_snapshot && project_snapshot->version() != *version) {
        return std::unexpected(AccessConfigError{
                .code = AccessConfigErrorCode::InvalidCombination,
                .field = "version",
                .message = "compiled snapshot version does not match the prepared update",
        });
    }
    if (!project_snapshot) {
        return PreparedProjectUpdate(ConfigUpdateStatus::Unloaded, std::string(project), *version, {});
    }

    auto bound = bind_project_service_selectors(*project_snapshot, selector_factory_);
    if (!bound) {
        return std::unexpected(std::move(bound.error()));
    }

    return PreparedProjectUpdate(ConfigUpdateStatus::Published, std::string(project), *version,
                                 std::make_shared<const ProjectRouteSnapshot>(std::move(*project_snapshot)));
}

ConfigUpdateOutcome RouteConfigStore::apply(std::string_view project, const std::optional<ProjectConfig> &config) {
    auto prepared = prepare(project, config);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    auto ready = std::move(*prepared).try_ready();
    if (!ready) {
        return std::unexpected(AccessConfigError{
                .code = AccessConfigErrorCode::InvalidCombination,
                .field = "service",
                .message = "service routes must complete wait_ready before publication",
        });
    }
    return commit(std::move(*ready));
}

ConfigUpdateOutcome RouteConfigStore::commit(ReadyProjectUpdate ready) {
    FIBER_ASSERT(ready.valid_);
    if (ready.status_ == ConfigUpdateStatus::IgnoredEmpty || ready.status_ == ConfigUpdateStatus::VersionUnchanged) {
        ready.valid_ = false;
        return ConfigUpdateResult{
                .status = ready.status_,
                .snapshot = pin(),
        };
    }
    FIBER_ASSERT(ready.status_ == ConfigUpdateStatus::Published || ready.status_ == ConfigUpdateStatus::Unloaded);

    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = registry_.loaded_snapshots();
    apply_to_candidate(candidate, ready);
    std::chrono::nanoseconds global_build_duration{};
    auto snapshot = build_candidate(candidate, global_build_duration);
    if (!snapshot) {
        ready.valid_ = false;
        return std::unexpected(std::move(snapshot.error()));
    }

    const SteadyClock::time_point publish_started = SteadyClock::now();
    auto published = std::make_shared<const AccessRouteSnapshot>(std::move(*snapshot));
    if (ready.status_ == ConfigUpdateStatus::Published) {
        FIBER_ASSERT(ready.project_snapshot_ != nullptr);
        registry_.replace(ready.project_, ready.version_, std::move(ready.project_snapshot_));
    } else {
        registry_.unload(ready.project_);
    }
    ready.valid_ = false;
    publisher_.publish(published);
    const std::chrono::nanoseconds publish_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - publish_started);
    return ConfigUpdateResult{
            .status = ready.status_,
            .snapshot = std::move(published),
            .global_build_duration = global_build_duration,
            .publish_duration = publish_duration,
    };
}

ConfigBatchUpdateOutcome RouteConfigStore::commit_batch(std::vector<ReadyProjectUpdate> ready) {
    std::vector<std::size_t> order(ready.size());
    std::iota(order.begin(), order.end(), 0U);
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        FIBER_ASSERT(ready[left].valid_);
        FIBER_ASSERT(ready[right].valid_);
        return ready[left].project_ < ready[right].project_;
    });
    for (std::size_t index = 1; index < order.size(); ++index) {
        if (ready[order[index - 1]].project_ == ready[order[index]].project_) {
            return std::unexpected(duplicate_batch_project_error());
        }
    }
    std::vector<ReadyProjectUpdate> sorted;
    sorted.reserve(ready.size());
    for (const std::size_t index: order) {
        sorted.push_back(std::move(ready[index]));
    }
    ready = std::move(sorted);

    ConfigBatchUpdateResult result;
    result.projects.reserve(ready.size());
    if (ready.empty()) {
        result.snapshot = pin();
        return result;
    }

    std::vector<bool> accepted(ready.size(), true);
    std::vector<std::optional<AccessConfigError>> failures(ready.size());
    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = registry_.loaded_snapshots();
    std::size_t loaded_projects = candidate.size();
    std::size_t host_capacity = 0;
    for (const std::shared_ptr<const ProjectRouteSnapshot> &project: candidate) {
        host_capacity += project->hosts().size();
    }
    for (const ReadyProjectUpdate &update: ready) {
        if (update.project_snapshot_) {
            host_capacity += update.project_snapshot_->hosts().size();
        }
    }
    std::unordered_set<std::string_view, HostPatternIdentityHash, HostPatternIdentityEqual> hosts;
    hosts.reserve(host_capacity);
    for (const std::shared_ptr<const ProjectRouteSnapshot> &project: candidate) {
        for (const CompiledHost &host: project->hosts()) {
            const bool inserted = hosts.insert(host.pattern).second;
            FIBER_ASSERT(inserted);
        }
    }

    bool has_mutation = false;
    for (std::size_t index = 0; index < ready.size(); ++index) {
        const ReadyProjectUpdate &update = ready[index];
        FIBER_ASSERT(update.valid_);
        if (update.status_ != ConfigUpdateStatus::Published && update.status_ != ConfigUpdateStatus::Unloaded) {
            continue;
        }

        const ProjectRouteSnapshot *previous = registry_.find_snapshot(update.project_);
        if (previous) {
            FIBER_ASSERT(loaded_projects != 0);
            --loaded_projects;
            for (const CompiledHost &host: previous->hosts()) {
                const std::size_t removed = hosts.erase(host.pattern);
                FIBER_ASSERT(removed == 1);
            }
        }

        if (update.status_ == ConfigUpdateStatus::Unloaded) {
            apply_to_candidate(candidate, update);
            has_mutation = true;
            continue;
        }

        FIBER_ASSERT(update.project_snapshot_ != nullptr);
        if (loaded_projects >= kAccessConfigLimits.project_list.max_projects) {
            accepted[index] = false;
            failures[index].emplace(project_count_error());
        } else {
            for (const CompiledHost &host: update.project_snapshot_->hosts()) {
                if (hosts.contains(host.pattern)) {
                    accepted[index] = false;
                    failures[index].emplace(duplicate_host_error(host.pattern));
                    break;
                }
            }
        }

        if (!accepted[index]) {
            if (previous) {
                ++loaded_projects;
                for (const CompiledHost &host: previous->hosts()) {
                    const bool inserted = hosts.insert(host.pattern).second;
                    FIBER_ASSERT(inserted);
                }
            }
            continue;
        }

        ++loaded_projects;
        for (const CompiledHost &host: update.project_snapshot_->hosts()) {
            const bool inserted = hosts.insert(host.pattern).second;
            FIBER_ASSERT(inserted);
        }
        apply_to_candidate(candidate, update);
        has_mutation = true;
    }

    std::optional<AccessRouteSnapshot> final_snapshot;
    if (has_mutation) {
        auto built = build_candidate(candidate, result.global_build_duration);
        if (!built) {
            return std::unexpected(std::move(built.error()));
        }
        final_snapshot.emplace(std::move(*built));
    }

    std::shared_ptr<const AccessRouteSnapshot> published;
    if (has_mutation) {
        FIBER_ASSERT(final_snapshot.has_value());
        const SteadyClock::time_point publish_started = SteadyClock::now();
        published = std::make_shared<const AccessRouteSnapshot>(std::move(*final_snapshot));
        for (std::size_t index = 0; index < ready.size(); ++index) {
            ReadyProjectUpdate &update = ready[index];
            if (!accepted[index]) {
                continue;
            }
            if (update.status_ == ConfigUpdateStatus::Published) {
                FIBER_ASSERT(update.project_snapshot_ != nullptr);
                registry_.replace(update.project_, update.version_, std::move(update.project_snapshot_));
            } else if (update.status_ == ConfigUpdateStatus::Unloaded) {
                registry_.unload(update.project_);
            }
        }
        publisher_.publish(published);
        result.publish_duration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - publish_started);
        result.published = true;
    } else {
        published = pin();
    }

    result.snapshot = std::move(published);
    for (std::size_t index = 0; index < ready.size(); ++index) {
        ReadyProjectUpdate &update = ready[index];
        update.valid_ = false;
        if (accepted[index]) {
            result.projects.push_back(ConfigBatchProjectResult{
                    .project = std::move(update.project_),
                    .outcome = update.status_,
            });
        } else {
            FIBER_ASSERT(failures[index].has_value());
            result.projects.push_back(ConfigBatchProjectResult{
                    .project = std::move(update.project_),
                    .outcome = std::unexpected(std::move(*failures[index])),
            });
        }
    }
    return result;
}

ConfigUpdateOutcome RouteConfigStore::remove_project(std::string_view project) {
    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = registry_.loaded_snapshots();
    const auto existing = std::lower_bound(candidate.begin(), candidate.end(), project,
                                           [](const std::shared_ptr<const ProjectRouteSnapshot> &entry,
                                              std::string_view name) { return entry->project() < name; });
    if (existing != candidate.end() && (*existing)->project() == project) {
        candidate.erase(existing);
    }

    std::chrono::nanoseconds global_build_duration{};
    auto snapshot = build_candidate(candidate, global_build_duration);
    if (!snapshot) {
        return std::unexpected(std::move(snapshot.error()));
    }

    const SteadyClock::time_point publish_started = SteadyClock::now();
    auto published = std::make_shared<const AccessRouteSnapshot>(std::move(*snapshot));
    registry_.remove(project);
    publisher_.publish(published);
    const std::chrono::nanoseconds publish_duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - publish_started);
    return ConfigUpdateResult{
            .status = ConfigUpdateStatus::ProjectRemoved,
            .snapshot = std::move(published),
            .global_build_duration = global_build_duration,
            .publish_duration = publish_duration,
    };
}

void RouteConfigStore::clear() noexcept {
    registry_.clear();
    publisher_.publish(std::make_shared<const AccessRouteSnapshot>());
}

std::optional<std::int32_t> RouteConfigStore::current_version(std::string_view project) const noexcept {
    return registry_.current_version(project);
}

AccessRouteSnapshotProvider RouteConfigStore::snapshot_provider() const noexcept {
    return AccessRouteSnapshotProvider{
            .context = this,
            .pin = [](const void *context) noexcept { return static_cast<const RouteConfigStore *>(context)->pin(); },
    };
}

void RouteConfigStore::apply_to_candidate(std::vector<std::shared_ptr<const ProjectRouteSnapshot>> &candidate,
                                          const ReadyProjectUpdate &ready) {
    const auto existing = std::lower_bound(candidate.begin(), candidate.end(), ready.project_,
                                           [](const std::shared_ptr<const ProjectRouteSnapshot> &entry,
                                              std::string_view project) { return entry->project() < project; });
    const bool found = existing != candidate.end() && (*existing)->project() == ready.project_;
    if (ready.status_ == ConfigUpdateStatus::Unloaded) {
        if (found) {
            candidate.erase(existing);
        }
        return;
    }
    if (ready.status_ != ConfigUpdateStatus::Published) {
        return;
    }
    FIBER_ASSERT(ready.project_snapshot_ != nullptr);
    if (found) {
        *existing = ready.project_snapshot_;
    } else {
        candidate.insert(existing, ready.project_snapshot_);
    }
}

} // namespace fiber::access_server
