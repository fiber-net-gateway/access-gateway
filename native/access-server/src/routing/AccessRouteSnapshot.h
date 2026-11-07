#ifndef FIBER_ACCESS_SERVER_ACCESS_ROUTE_SNAPSHOT_H
#define FIBER_ACCESS_SERVER_ACCESS_ROUTE_SNAPSHOT_H

#include "EdgeFunctionsSnapshot.h"
#include "ProjectRouteSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fiber::access_server {

struct ProjectHostMatch {
    const ProjectRouteSnapshot *project = nullptr;
    const CompiledHost *host = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return project != nullptr; }
};

class AccessRouteSnapshot {
public:
    AccessRouteSnapshot() = default;

    // edge_functions (when set) participates in the joint mount table so the
    // glob expansion is recomputed against exactly this snapshot's projects.
    [[nodiscard]] static std::expected<AccessRouteSnapshot, AccessConfigError>
    build(std::span<const std::shared_ptr<const ProjectRouteSnapshot>> projects,
          std::shared_ptr<const EdgeFunctionsState> edge_functions = {});

    [[nodiscard]] ProjectHostMatch match_host(std::string_view host) const noexcept;
    [[nodiscard]] const std::vector<std::shared_ptr<const ProjectRouteSnapshot>> &projects() const noexcept {
        return projects_;
    }
    [[nodiscard]] std::size_t host_count() const noexcept { return host_count_; }
    [[nodiscard]] std::size_t route_count() const noexcept { return route_count_; }
    [[nodiscard]] std::size_t compiled_program_count() const noexcept { return compiled_program_count_; }
    [[nodiscard]] std::size_t estimated_memory_bytes() const noexcept { return estimated_memory_bytes_; }
    [[nodiscard]] std::size_t static_response_bytes() const noexcept { return static_response_bytes_; }

    // Mounted edge functions by project (empty table when none are
    // configured); the lookup itself is allocation-free.
    [[nodiscard]] const EdgeFunctionMountTable &edge_functions() const noexcept;
    [[nodiscard]] std::size_t edge_function_count() const noexcept { return edge_function_count_; }
    // Verified script payload bytes across the active edge-function set.
    [[nodiscard]] std::size_t edge_function_script_bytes() const noexcept {
        return edge_functions_ ? edge_functions_->total_script_bytes : 0U;
    }

private:
    struct HostTarget {
        std::uint32_t project_index = 0;
        std::uint32_t host_index = 0;
    };

    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects_;
    std::vector<HostTarget> host_targets_;
    HostMatcher host_matcher_;
    // Keeps the compiled programs alive; the mount table borrows from it.
    std::shared_ptr<const EdgeFunctionsState> edge_functions_;
    std::shared_ptr<const EdgeFunctionMountTable> edge_mounts_;
    std::size_t host_count_ = 0;
    std::size_t route_count_ = 0;
    std::size_t compiled_program_count_ = 0;
    std::size_t estimated_memory_bytes_ = 0;
    std::size_t static_response_bytes_ = 0;
    std::size_t edge_function_count_ = 0;
};

struct AccessRouteSnapshotProvider {
    using PinFunction = std::shared_ptr<const AccessRouteSnapshot> (*)(const void *context) noexcept;

    const void *context = nullptr;
    PinFunction pin = nullptr;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_ROUTE_SNAPSHOT_H
