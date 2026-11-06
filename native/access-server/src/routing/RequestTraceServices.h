#ifndef FIBER_ACCESS_SERVER_REQUEST_TRACE_SERVICES_H
#define FIBER_ACCESS_SERVER_REQUEST_TRACE_SERVICES_H

#include "TraceConstSource.h"

#include <chrono>
#include <memory>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/http_script/HttpScriptServices.h>

namespace fiber::access_server {

// Per-request HttpScriptServices installed on the ScriptExchangeCtx for the
// whole request lifetime. It exposes the trace state to the $context.<key> and
// $header.traceparent constants (TraceContextConstExtension) and delegates
// upstream acquisition to the worker-owned script HTTP services once those are
// attached (tests and validator-only paths never attach one, so acquire fails
// closed exactly like a null services pointer).
//
// Installation invariant: in production this is the only services object set
// on a request's ScriptExchangeCtx outside EdgeFunctionExecutor's per-function
// swap, which restores the previous pointer afterwards. The extension
// static_casts services() back to this type; that cast is sound because the
// only compiled programs referencing the extension's constants come from
// project compilation (conditions, scripts, templates), and those never
// execute while the edge services pointer is installed.
class RequestTraceServices final : public http_script::HttpScriptServices {
public:
    explicit RequestTraceServices(const TraceConstSource &source) noexcept : trace_source_(&source) {}

    RequestTraceServices(const RequestTraceServices &) = delete;
    RequestTraceServices &operator=(const RequestTraceServices &) = delete;
    RequestTraceServices(RequestTraceServices &&) = delete;
    RequestTraceServices &operator=(RequestTraceServices &&) = delete;

    // The worker attaches its script HTTP services after telemetry
    // construction; acquire() stays fail-closed until then.
    void attach_upstream(http_script::HttpScriptServices &upstream) noexcept { upstream_ = &upstream; }

    [[nodiscard]] const TraceConstSource *trace_source() const noexcept { return trace_source_; }

    fiber::async::Task<fiber::common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
    acquire(const http_script::HttpTargetSpec &target, std::chrono::milliseconds connect_timeout) noexcept override;

private:
    http_script::HttpScriptServices *upstream_ = nullptr;
    const TraceConstSource *trace_source_ = nullptr;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_REQUEST_TRACE_SERVICES_H
