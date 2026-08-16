#include "AccessConfigCodec.h"

#include "AccessConfigCoercion.h"
#include "AccessConfigErrorBuilder.h"

#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

ProjectListResult parse_project_list(std::string_view content, const AccessConfigLimits &limits) {
    if (content.size() > limits.project_list.max_payload_bytes) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::LimitExceeded, "projects",
                                                         "project list payload exceeds the configured byte limit"));
    }
    if (content.empty()) {
        return std::vector<std::string>{};
    }

    content = config_detail::trim_java(content);
    if (content.empty()) {
        // Java "".split(";") returns one empty element.
        return std::vector<std::string>{std::string()};
    }

    std::vector<std::string> projects;
    std::size_t offset = 0;
    while (true) {
        if (projects.size() == limits.project_list.max_projects) {
            // Java String.split drops all trailing empty elements. Preserve
            // that behavior at the exact entry limit without allocating an
            // unbounded number of empty strings for repeated separators.
            if (content.find_first_not_of(';', offset) == std::string_view::npos) {
                break;
            }
            return std::unexpected(config_detail::make_error(AccessConfigErrorCode::LimitExceeded, "projects",
                                                             "project count exceeds the configured limit"));
        }
        const std::size_t separator = content.find(';', offset);
        const std::string_view project = separator == std::string_view::npos
                                                 ? content.substr(offset)
                                                 : content.substr(offset, separator - offset);
        if (project.size() > limits.project_list.max_project_name_bytes) {
            return std::unexpected(config_detail::make_error(AccessConfigErrorCode::LimitExceeded,
                                                             config_detail::index_path("projects", projects.size()),
                                                             "project name exceeds the configured byte limit"));
        }
        if (separator == std::string_view::npos) {
            projects.emplace_back(project);
            break;
        }
        projects.emplace_back(project);
        offset = separator + 1;
    }
    while (!projects.empty() && projects.back().empty()) {
        projects.pop_back();
    }
    return projects;
}

} // namespace fiber::access_server
