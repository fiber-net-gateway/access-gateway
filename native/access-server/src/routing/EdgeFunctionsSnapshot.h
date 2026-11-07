#ifndef FIBER_ACCESS_SERVER_EDGE_FUNCTIONS_SNAPSHOT_H
#define FIBER_ACCESS_SERVER_EDGE_FUNCTIONS_SNAPSHOT_H

#include "../config/AccessConfig.h"
#include "../config/AccessConfigLimits.h"
#include "EdgeFunctionLibrary.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

// Watcher output: one parsed index plus the compiled programs it references,
// aligned by declaration order (programs[i] belongs to index.functions[i]).
// Immutable once assembled; the mount table keeps these alive.
struct EdgeFunctionsState {
    EdgeFunctionIndexConfig index;
    // Mutable pointees like route scripts' shared_ptr<Script>: exec_async is a
    // non-const member even though it only reads the compiled bytecode, and
    // concurrent executions share one program safely (each request owns its
    // heap and exchange context).
    std::vector<std::shared_ptr<EdgeFunctionProgram>> programs;
    std::uint64_t total_script_bytes = 0;
};

// One function mounted on one project, ready for request-time execution. The
// chain a mounted entry belongs to carries its phase: mounted_for() returns
// the request_head chain, mounted_body_for() the request_body chain.
struct EdgeFunctionMounted {
    std::string_view name; // borrowed from the table's storage
    EdgeFunctionProgram *program; // borrowed; the table owns the shared_ptr
    std::int32_t priority = 0;
    // Sticky sampling threshold from the matching mount (design §3.1): the
    // executor compares one per-request crc32(peer IP) bucket against it.
    std::int32_t ratio = kEdgeFunctionFullRatio;
};

// Joint index (docs/edge-functions-and-waf-design.md §3.6): the
// project-glob expansion happens once per snapshot assembly, never on the
// request path. Request-time lookup is one binary search plus a span; a
// project with no mounted functions returns an empty span and the handler
// skips the whole edge-function path.
class EdgeFunctionMountTable final {
public:
    // Expands every function's mounts against the published project names.
    // Per project, functions run in priority order (ascending), ties broken
    // by index declaration order; the first mount (declaration order) that
    // matches a project decides that function's priority for it.
    [[nodiscard]] static std::shared_ptr<const EdgeFunctionMountTable>
    build(const EdgeFunctionsState &state, std::span<const std::string_view> project_names);

    // Sorted-project lookup; allocation-free. Head chain (runs before route
    // matching) and body chain (runs for opted-in routes after the body was
    // prefetched), each in priority order with declaration-order ties.
    [[nodiscard]] std::span<const EdgeFunctionMounted> mounted_for(std::string_view project) const noexcept;
    [[nodiscard]] std::span<const EdgeFunctionMounted> mounted_body_for(std::string_view project) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t mounted_project_count() const noexcept { return project_names_.size(); }

    EdgeFunctionMountTable() = default;
    EdgeFunctionMountTable(const EdgeFunctionMountTable &) = delete;
    EdgeFunctionMountTable &operator=(const EdgeFunctionMountTable &) = delete;
    EdgeFunctionMountTable(EdgeFunctionMountTable &&) noexcept = default;
    EdgeFunctionMountTable &operator=(EdgeFunctionMountTable &&) noexcept = default;

private:
    // Keeps every compiled program (and its cidr directive definitions) alive
    // for exactly as long as snapshots referencing this table.
    std::vector<std::shared_ptr<EdgeFunctionProgram>> owned_programs_;
    std::vector<std::string> name_storage_;
    std::vector<EdgeFunctionMounted> entries_;
    std::vector<std::uint32_t> group_offsets_;
    // Per project, where the request_body chain begins inside the group span.
    std::vector<std::uint32_t> body_offsets_;
    std::vector<std::string_view> project_names_;
};

// Glob semantics for project-name selectors: '*' matches any run (including
// empty) of bytes, every other byte matches literally. Case-sensitive, no
// character classes — the same shapes the console validates for mounts.
[[nodiscard]] bool edge_glob_matches(std::string_view pattern, std::string_view value) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_EDGE_FUNCTIONS_SNAPSHOT_H
