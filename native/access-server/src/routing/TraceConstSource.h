#ifndef FIBER_ACCESS_SERVER_TRACE_CONST_SOURCE_H
#define FIBER_ACCESS_SERVER_TRACE_CONST_SOURCE_H

#include <optional>
#include <string_view>

namespace fiber::access_server {

// Read-only view of the per-request trace state for script constants
// ($context.<key> and $header.traceparent). Implemented by TracePropagation
// (observability layer) and consumed through RequestTraceServices at request
// time; the interface lives in the config layer, next to the script
// compilation extension that references it, so neither side needs a reverse
// component dependency.
class TraceConstSource {
public:
    // `normalized_key` is already normalized (ASCII lowercase, '-' -> '_',
    // same rule as fiber/http_script/ConstPackage.h); implementations fold
    // their stored keys through the same rule before comparing.
    [[nodiscard]] virtual std::optional<std::string_view>
    resolve_trace_context(std::string_view normalized_key) const noexcept = 0;

    // The request's resolved W3C trace parent: the incoming traceparent header
    // when present, otherwise the locally synthesized one.
    [[nodiscard]] virtual std::string_view trace_parent() const noexcept = 0;

protected:
    ~TraceConstSource() = default;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TRACE_CONST_SOURCE_H
