#include "ProjectSnapshotRegistry.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

std::optional<std::int32_t> ProjectSnapshotRegistry::current_version(std::string_view project) const noexcept {
    const auto existing = projects_.find(project);
    return existing == projects_.end() ? std::nullopt : existing->second.version;
}

const ProjectRouteSnapshot *ProjectSnapshotRegistry::find_snapshot(std::string_view project) const noexcept {
    const auto existing = projects_.find(project);
    return existing == projects_.end() ? nullptr : existing->second.snapshot.get();
}

std::vector<std::shared_ptr<const ProjectRouteSnapshot>> ProjectSnapshotRegistry::loaded_snapshots() const {
    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> result;
    result.reserve(projects_.size());
    for (const auto &[project, record]: projects_) {
        (void) project;
        if (record.snapshot) {
            result.push_back(record.snapshot);
        }
    }
    return result;
}

void ProjectSnapshotRegistry::replace(std::string_view project, std::int32_t version,
                                      std::shared_ptr<const ProjectRouteSnapshot> snapshot) {
    FIBER_ASSERT(snapshot);
    FIBER_ASSERT(snapshot->project() == project);
    FIBER_ASSERT(snapshot->version() == version);
    ProjectRecord &record = projects_[std::string(project)];
    record.version = version;
    record.snapshot = std::move(snapshot);
}

void ProjectSnapshotRegistry::unload(std::string_view project) noexcept {
    const auto existing = projects_.find(project);
    if (existing != projects_.end()) {
        // Java ListenerWrap retains the last successful non-empty ProjectConf
        // version when an empty Host map unloads a project.
        existing->second.snapshot.reset();
    }
}

void ProjectSnapshotRegistry::remove(std::string_view project) noexcept {
    const auto existing = projects_.find(project);
    if (existing != projects_.end()) {
        projects_.erase(existing);
    }
}

void ProjectSnapshotRegistry::clear() noexcept { projects_.clear(); }

} // namespace fiber::access_server
