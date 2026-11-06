#include "EdgeFunctionsSnapshot.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace fiber::access_server {
namespace {

// Classic single-star glob matching with backtracking. Patterns are bounded
// (limits.max_glob_bytes), so the backtracking loop is bounded by pattern
// length; no allocation.
bool star_glob(std::string_view pattern, std::string_view value) noexcept {
    std::size_t pattern_pos = 0;
    std::size_t value_pos = 0;
    std::size_t star_pattern = std::string_view::npos;
    std::size_t star_value = 0;
    while (value_pos < value.size()) {
        if (pattern_pos < pattern.size() && (pattern[pattern_pos] == '*' || pattern[pattern_pos] == value[value_pos])) {
            if (pattern[pattern_pos] == '*') {
                star_pattern = pattern_pos++;
                star_value = value_pos;
            } else {
                ++pattern_pos;
                ++value_pos;
            }
        } else if (star_pattern != std::string_view::npos) {
            pattern_pos = star_pattern + 1;
            value_pos = ++star_value;
        } else {
            return false;
        }
    }
    while (pattern_pos < pattern.size() && pattern[pattern_pos] == '*') {
        ++pattern_pos;
    }
    return pattern_pos == pattern.size();
}

// First mount (declaration order) whose selector matches the project decides
// the mount; later mounts of the same function are ignored for that project.
const EdgeFunctionMountConfig *matching_mount(const EdgeFunctionIndexEntry &function, std::string_view project) {
    for (const EdgeFunctionMountConfig &mount: function.mounts) {
        bool included = false;
        for (const std::string &glob: mount.projects.include) {
            if (star_glob(glob, project)) {
                included = true;
                break;
            }
        }
        if (!included) {
            continue;
        }
        bool excluded = false;
        for (const std::string &glob: mount.projects.exclude) {
            if (star_glob(glob, project)) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            return &mount;
        }
    }
    return nullptr;
}

} // namespace

bool edge_glob_matches(std::string_view pattern, std::string_view value) noexcept { return star_glob(pattern, value); }

std::shared_ptr<const EdgeFunctionMountTable>
EdgeFunctionMountTable::build(const EdgeFunctionsState &state, std::span<const std::string_view> project_names) {
    auto table = std::make_shared<EdgeFunctionMountTable>();
    if (state.index.functions.empty() || project_names.empty()) {
        return table;
    }

    table->owned_programs_ = state.programs;
    table->name_storage_.reserve(state.index.functions.size());
    for (const EdgeFunctionIndexEntry &function: state.index.functions) {
        table->name_storage_.push_back(function.name);
    }

    // Per project: the mounted functions in declaration order, then a stable
    // sort by priority so equal priorities keep declaration order.
    struct ProjectGroup {
        std::string_view project;
        std::vector<EdgeFunctionMounted> mounted;
    };
    std::vector<ProjectGroup> groups;
    groups.reserve(project_names.size());
    for (const std::string_view project: project_names) {
        ProjectGroup group{.project = project, .mounted = {}};
        for (std::size_t function_index = 0; function_index < state.index.functions.size(); ++function_index) {
            const EdgeFunctionIndexEntry &function = state.index.functions[function_index];
            const EdgeFunctionMountConfig *mount = matching_mount(function, project);
            if (mount == nullptr) {
                continue;
            }
            group.mounted.push_back(EdgeFunctionMounted{
                    .name = table->name_storage_[function_index],
                    .program = table->owned_programs_[function_index].get(),
                    .priority = mount->priority,
                    .ratio = mount->ratio,
            });
        }
        if (!group.mounted.empty()) {
            std::stable_sort(group.mounted.begin(), group.mounted.end(),
                             [](const EdgeFunctionMounted &left, const EdgeFunctionMounted &right) noexcept {
                                 return left.priority < right.priority;
                             });
            groups.push_back(std::move(group));
        }
    }

    std::sort(groups.begin(), groups.end(), [](const ProjectGroup &left, const ProjectGroup &right) noexcept {
        return left.project < right.project;
    });

    std::size_t entry_count = 0;
    for (const ProjectGroup &group: groups) {
        entry_count += group.mounted.size();
    }
    table->entries_.reserve(entry_count);
    table->group_offsets_.reserve(groups.size() + 1U);
    table->project_names_.reserve(groups.size());
    table->group_offsets_.push_back(0);
    for (ProjectGroup &group: groups) {
        table->project_names_.push_back(group.project);
        for (EdgeFunctionMounted &mounted: group.mounted) {
            table->entries_.push_back(mounted);
        }
        table->group_offsets_.push_back(static_cast<std::uint32_t>(table->entries_.size()));
    }
    return table;
}

std::span<const EdgeFunctionMounted> EdgeFunctionMountTable::mounted_for(std::string_view project) const noexcept {
    const auto found = std::lower_bound(project_names_.begin(), project_names_.end(), project);
    if (found == project_names_.end() || *found != project) {
        return {};
    }
    const std::size_t group = static_cast<std::size_t>(found - project_names_.begin());
    const std::uint32_t begin = group_offsets_[group];
    const std::uint32_t end = group_offsets_[group + 1U];
    return {entries_.data() + begin, end - begin};
}

} // namespace fiber::access_server
