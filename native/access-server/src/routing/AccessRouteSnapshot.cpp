#include "AccessRouteSnapshot.h"

#include <string>
#include <unordered_set>

namespace fiber::access_server {
namespace {

AccessConfigError snapshot_error(std::string_view field, std::string_view message,
                                 AccessConfigErrorCode code = AccessConfigErrorCode::Conflict) {
    return AccessConfigError{
            .code = code,
            .field = std::string(field),
            .message = std::string(message),
    };
}

} // namespace

std::expected<AccessRouteSnapshot, AccessConfigError>
AccessRouteSnapshot::build(std::span<const std::shared_ptr<const ProjectRouteSnapshot>> projects) {
    if (projects.size() > kAccessConfigLimits.project_list.max_projects) {
        return std::unexpected(snapshot_error("projects", "project count exceeds the configured limit",
                                              AccessConfigErrorCode::LimitExceeded));
    }
    AccessRouteSnapshot snapshot;
    snapshot.projects_.reserve(projects.size());
    std::unordered_set<std::string_view> project_names;
    project_names.reserve(projects.size());

    std::size_t host_count = 0;
    for (const std::shared_ptr<const ProjectRouteSnapshot> &project: projects) {
        if (!project) {
            return std::unexpected(snapshot_error("projects", "project snapshot is null"));
        }
        if (!project_names.insert(project->project()).second) {
            return std::unexpected(snapshot_error("projects", "project name is duplicate"));
        }
        host_count += project->hosts().size();
        snapshot.route_count_ += project->routes().size();
        snapshot.compiled_program_count_ += project->compiled_program_count();
        snapshot.estimated_memory_bytes_ += project->estimated_memory_bytes();
        snapshot.static_response_bytes_ += project->static_response_bytes();
        snapshot.projects_.push_back(project);
    }
    snapshot.host_count_ = host_count;

    std::vector<HostPattern> patterns;
    patterns.reserve(host_count);
    snapshot.host_targets_.reserve(host_count);
    for (std::uint32_t project_index = 0; project_index < snapshot.projects_.size(); ++project_index) {
        const auto &hosts = snapshot.projects_[project_index]->hosts();
        for (std::uint32_t host_index = 0; host_index < hosts.size(); ++host_index) {
            const std::uint32_t target = static_cast<std::uint32_t>(snapshot.host_targets_.size());
            snapshot.host_targets_.push_back(HostTarget{
                    .project_index = project_index,
                    .host_index = host_index,
            });
            patterns.push_back(HostPattern{
                    .pattern = hosts[host_index].pattern,
                    .handler = target,
            });
        }
    }

    auto matcher = HostMatcher::build(patterns);
    if (!matcher) {
        return std::unexpected(std::move(matcher.error()));
    }
    snapshot.host_matcher_ = std::move(*matcher);
    return snapshot;
}

ProjectHostMatch AccessRouteSnapshot::match_host(std::string_view host) const noexcept {
    const std::optional<std::uint32_t> target_index = host_matcher_.match(host);
    if (!target_index) {
        return {};
    }
    const HostTarget &target = host_targets_[*target_index];
    const ProjectRouteSnapshot *project = projects_[target.project_index].get();
    return ProjectHostMatch{
            .project = project,
            .host = &project->hosts()[target.host_index],
    };
}

} // namespace fiber::access_server
