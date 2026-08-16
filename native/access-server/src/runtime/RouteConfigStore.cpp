#include "RouteConfigStore.h"

#include <cstddef>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

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
    script_compiler_(script_compiler), selector_factory_(selector_factory) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#endif
}

RouteConfigStore::RouteConfigStore(ScriptCompilerAdapter script_compiler, AccessServiceDiscovery &service_discovery,
                                   AccessServiceDiscoveryOptions discovery_options) :
    script_compiler_(script_compiler), service_selector_factory_(service_discovery, std::move(discovery_options)),
    selector_factory_(service_selector_factory_.adapter()), uses_service_discovery_(true) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#endif
}

PreparedProjectUpdateOutcome RouteConfigStore::prepare(std::string_view project,
                                                       const std::optional<ProjectConfig> &config) {
    if (!config) {
        return prepare_compiled(project, std::nullopt, std::nullopt);
    }

    const std::optional<std::int32_t> current_version = this->current_version(project);
    if (current_version && *current_version == config->version) {
        return PreparedProjectUpdate(ConfigUpdateStatus::VersionUnchanged, std::string(project), config->version, {});
    }

    auto compiled = compile_project_config(project, *config, script_compiler_);
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

    if (uses_service_discovery_) {
        service_selector_factory_.begin_compile();
    }
    auto bound = bind_project_service_selectors(*project_snapshot, selector_factory_);
    std::optional<nacos::NamingServiceError> acquire_error;
    if (uses_service_discovery_) {
        acquire_error = service_selector_factory_.take_error();
    }
    if (!bound) {
        return std::unexpected(std::move(bound.error()));
    }
    if (acquire_error) {
        return std::unexpected(AccessConfigError{
                .code = AccessConfigErrorCode::InvalidCombination,
                .field = "service",
                .message = std::move(acquire_error->message),
        });
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
        return ConfigUpdateResult{
                .status = ready.status_,
                .snapshot = pin(),
        };
    }
    FIBER_ASSERT(ready.status_ == ConfigUpdateStatus::Published || ready.status_ == ConfigUpdateStatus::Unloaded);

    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = projects_;
    std::size_t existing = candidate.size();
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        if (candidate[i]->project() == ready.project_) {
            existing = i;
            break;
        }
    }

    if (ready.status_ == ConfigUpdateStatus::Unloaded) {
        if (existing != candidate.size()) {
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(existing));
        }
        // Java ListenerWrap leaves its last successful non-empty ProjectConf
        // version unchanged when an empty Host map unloads a project.
        return publish_candidate(std::move(candidate), ConfigUpdateStatus::Unloaded);
    }

    FIBER_ASSERT(ready.project_snapshot_ != nullptr);
    if (existing == candidate.size()) {
        candidate.push_back(std::move(ready.project_snapshot_));
    } else {
        candidate[existing] = std::move(ready.project_snapshot_);
    }

    auto published = publish_candidate(std::move(candidate), ConfigUpdateStatus::Published);
    if (published) {
        set_published_version(ready.project_, ready.version_);
    }
    return published;
}

ConfigUpdateOutcome RouteConfigStore::remove_project(std::string_view project) {
    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = projects_;
    for (auto iterator = candidate.begin(); iterator != candidate.end(); ++iterator) {
        if ((*iterator)->project() == project) {
            candidate.erase(iterator);
            break;
        }
    }

    auto published = publish_candidate(std::move(candidate), ConfigUpdateStatus::ProjectRemoved);
    if (published) {
        remove_published_version(project);
    }
    return published;
}

void RouteConfigStore::clear() noexcept {
    projects_.clear();
    published_versions_.clear();
    auto empty = std::make_shared<const AccessRouteSnapshot>();
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(empty), std::memory_order_release);
#else
    std::atomic_store_explicit(&published_, std::move(empty), std::memory_order_release);
#endif
}

std::optional<std::int32_t> RouteConfigStore::current_version(std::string_view project) const noexcept {
    for (const PublishedVersion &entry: published_versions_) {
        if (entry.project == project) {
            return entry.version;
        }
    }
    return std::nullopt;
}

void RouteConfigStore::set_published_version(std::string_view project, std::int32_t version) {
    for (PublishedVersion &entry: published_versions_) {
        if (entry.project == project) {
            entry.version = version;
            return;
        }
    }
    published_versions_.push_back(PublishedVersion{
            .project = std::string(project),
            .version = version,
    });
}

void RouteConfigStore::remove_published_version(std::string_view project) {
    for (auto iterator = published_versions_.begin(); iterator != published_versions_.end(); ++iterator) {
        if (iterator->project == project) {
            published_versions_.erase(iterator);
            return;
        }
    }
}

ConfigUpdateOutcome
RouteConfigStore::publish_candidate(std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate,
                                    ConfigUpdateStatus status) {
    auto snapshot = AccessRouteSnapshot::build(candidate);
    if (!snapshot) {
        return std::unexpected(std::move(snapshot.error()));
    }

    auto published = std::make_shared<const AccessRouteSnapshot>(std::move(*snapshot));
    projects_ = std::move(candidate);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(published, std::memory_order_release);
#else
    std::atomic_store_explicit(&published_, published, std::memory_order_release);
#endif
    return ConfigUpdateResult{
            .status = status,
            .snapshot = std::move(published),
    };
}

} // namespace fiber::access_server
