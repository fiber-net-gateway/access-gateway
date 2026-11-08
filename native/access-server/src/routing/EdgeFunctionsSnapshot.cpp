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

bool selector_matches(const EdgeFunctionProjectsSelector &selector, std::string_view project) {
    const auto matches = [project](const std::string &glob) { return star_glob(glob, project); };
    return std::ranges::any_of(selector.include, matches) && std::ranges::none_of(selector.exclude, matches);
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

    // replacer_of[i] is the canary taking over function i, if any. The codec
    // guarantees at most one per target.
    std::vector<const EdgeFunctionIndexEntry *> replacer_of(state.index.functions.size(), nullptr);
    for (const EdgeFunctionIndexEntry &function: state.index.functions) {
        if (function.replaces.empty()) {
            continue;
        }
        const auto target = std::ranges::find(state.index.functions, function.replaces, &EdgeFunctionIndexEntry::name);
        if (target != state.index.functions.end()) {
            replacer_of[static_cast<std::size_t>(target - state.index.functions.begin())] = &function;
        }
    }

    // Per project: the mounted functions split into head and body chains
    // (declaration order), then a stable sort by priority per chain so equal
    // priorities keep declaration order.
    struct ProjectGroup {
        std::string_view project;
        std::vector<EdgeFunctionMounted> head;
        std::vector<EdgeFunctionMounted> body;
    };
    std::vector<ProjectGroup> groups;
    groups.reserve(project_names.size());
    for (const std::string_view project: project_names) {
        ProjectGroup group{.project = project, .head = {}, .body = {}};
        for (std::size_t function_index = 0; function_index < state.index.functions.size(); ++function_index) {
            const EdgeFunctionIndexEntry &function = state.index.functions[function_index];
            if (!selector_matches(function.projects, project)) {
                continue;
            }
            const EdgeFunctionIndexEntry *const replacer = replacer_of[function_index];
            const bool replaced = replacer != nullptr && selector_matches(replacer->projects, project);
            EdgeFunctionMounted mounted{
                    .name = table->name_storage_[function_index],
                    .program = table->owned_programs_[function_index].get(),
                    .priority = function.priority,
                    .bucket_begin = replaced ? replacer->ratio : 0,
                    .bucket_end = function.ratio,
            };
            if (function.phase == EdgeFunctionPhase::RequestBody) {
                group.body.push_back(mounted);
            } else {
                group.head.push_back(mounted);
            }
        }
        const auto by_priority = [](const EdgeFunctionMounted &left, const EdgeFunctionMounted &right) noexcept {
            return left.priority < right.priority;
        };
        if (!group.head.empty() || !group.body.empty()) {
            std::stable_sort(group.head.begin(), group.head.end(), by_priority);
            std::stable_sort(group.body.begin(), group.body.end(), by_priority);
            groups.push_back(std::move(group));
        }
    }

    std::sort(groups.begin(), groups.end(), [](const ProjectGroup &left, const ProjectGroup &right) noexcept {
        return left.project < right.project;
    });

    std::size_t entry_count = 0;
    for (const ProjectGroup &group: groups) {
        entry_count += group.head.size() + group.body.size();
    }
    table->entries_.reserve(entry_count);
    table->group_offsets_.reserve(groups.size() + 1U);
    table->body_offsets_.reserve(groups.size());
    table->project_names_.reserve(groups.size());
    table->group_offsets_.push_back(0);
    for (ProjectGroup &group: groups) {
        table->project_names_.push_back(group.project);
        for (EdgeFunctionMounted &mounted: group.head) {
            table->entries_.push_back(mounted);
        }
        // The body chain occupies the tail of the group span; body_offsets_
        // marks its start (equal to the span end when the chain is empty).
        table->body_offsets_.push_back(static_cast<std::uint32_t>(table->entries_.size()));
        for (EdgeFunctionMounted &mounted: group.body) {
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
    const std::uint32_t end = body_offsets_[group];
    return {entries_.data() + begin, end - begin};
}

std::span<const EdgeFunctionMounted> EdgeFunctionMountTable::mounted_body_for(std::string_view project) const noexcept {
    const auto found = std::lower_bound(project_names_.begin(), project_names_.end(), project);
    if (found == project_names_.end() || *found != project) {
        return {};
    }
    const std::size_t group = static_cast<std::size_t>(found - project_names_.begin());
    const std::uint32_t begin = body_offsets_[group];
    const std::uint32_t end = group_offsets_[group + 1U];
    return {entries_.data() + begin, end - begin};
}

} // namespace fiber::access_server
