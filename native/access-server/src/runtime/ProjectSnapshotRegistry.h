#ifndef FIBER_ACCESS_SERVER_PROJECT_SNAPSHOT_REGISTRY_H
#define FIBER_ACCESS_SERVER_PROJECT_SNAPSHOT_REGISTRY_H

#include "../routing/ProjectRouteSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

// Owner-loop-only source of truth for last successful Project versions and
// currently loaded immutable Project snapshots.
class ProjectSnapshotRegistry final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] std::optional<std::int32_t> current_version(std::string_view project) const noexcept;
    [[nodiscard]] const ProjectRouteSnapshot *find_snapshot(std::string_view project) const noexcept;
    [[nodiscard]] std::vector<std::shared_ptr<const ProjectRouteSnapshot>> loaded_snapshots() const;
    [[nodiscard]] bool empty() const noexcept { return projects_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return projects_.size(); }

    void replace(std::string_view project, std::int32_t version, std::shared_ptr<const ProjectRouteSnapshot> snapshot);
    void unload(std::string_view project) noexcept;
    void remove(std::string_view project) noexcept;
    void clear() noexcept;

private:
    struct ProjectRecord {
        std::optional<std::int32_t> version;
        std::shared_ptr<const ProjectRouteSnapshot> snapshot;
    };

    std::map<std::string, ProjectRecord, std::less<>> projects_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROJECT_SNAPSHOT_REGISTRY_H
