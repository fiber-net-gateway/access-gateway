#ifndef FIBER_ACCESS_SERVER_TRACE_CONTEXT_CONST_EXTENSION_H
#define FIBER_ACCESS_SERVER_TRACE_CONTEXT_CONST_EXTENSION_H

#include <map>
#include <string>
#include <string_view>

#include <fiber/script/Library.h>
#include <fiber/script/std/StdLibrary.h>

namespace fiber::access_server {

// Resolves the trace-context constant namespaces for project-compiled scripts:
//
//   $context.<key>   -> live trace context value (null when absent), written by
//                       the gray cluster decider and proxy trace-context config
//   $header.traceparent -> the resolved trace parent (incoming header or the
//                       locally synthesized one), overriding the raw request
//                       header exactly as the old prepare-time bind did
//
// Both resolve to synchronous constants reading the per-request
// RequestTraceServices installed on the ScriptExchangeCtx; values are resolved
// on every access, so put/remove_trace_context mid-request are observed
// immediately (the old implementation re-bound package slots to achieve the
// same liveness). Any $header.<other> falls through to RouteScriptExtension.
//
// Registered before RouteScriptExtension so the traceparent override wins the
// $header namespace. Only project compilation composes this extension; the
// edge-function library deliberately does not ($context stays out of the v1
// edge contract, and the edge executor swaps the services pointer).
class TraceContextConstExtension final {
public:
    using HostCallable = fiber::script::Library::HostCallable;
    using HostCallFrame = fiber::script::Library::HostCallFrame;

    TraceContextConstExtension() noexcept;

    TraceContextConstExtension(const TraceContextConstExtension &) = delete;
    TraceContextConstExtension &operator=(const TraceContextConstExtension &) = delete;
    TraceContextConstExtension(TraceContextConstExtension &&) = delete;
    TraceContextConstExtension &operator=(TraceContextConstExtension &&) = delete;

    [[nodiscard]] static const fiber::script::std_lib::StdLibrary::ExtOps &ops() noexcept;

private:
    [[nodiscard]] const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key);
    [[nodiscard]] static const HostCallable *resolve_constant_op(void *ctx, std::string_view namespace_name,
                                                                 std::string_view key) noexcept;
    static fiber::script::AbiResult context_constant_fn(void *userdata, const HostCallFrame &frame) noexcept;
    static fiber::script::AbiResult traceparent_constant_fn(void *userdata, const HostCallFrame &frame) noexcept;

    // Normalized key -> callable whose userdata points at the owning map key.
    // Map nodes are stable and the extension lives for the process, satisfying
    // the compiled-program userdata lifetime contract. Growth is bounded by
    // the distinct context-key vocabulary ever compiled.
    std::map<std::string, HostCallable> context_callables_;
    HostCallable traceparent_callable_{};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TRACE_CONTEXT_CONST_EXTENSION_H
