#ifndef FIBER_ACCESS_SERVER_EDGE_FUNCTION_EXECUTOR_H
#define FIBER_ACCESS_SERVER_EDGE_FUNCTION_EXECUTOR_H

#include "../routing/EdgeFunctionsSnapshot.h"

#include <span>
#include <string_view>

#include <fiber/async/Task.h>

namespace fiber::http_script {
class ScriptExchangeCtx;
}

namespace fiber::access_server {

class AccessRequestTelemetry;

// Executes the edge functions mounted for one request's project
// (docs/edge-functions-and-waf-design.md §3.2): after host policy, before
// route matching, in the snapshot's priority order. The first function that
// writes a response short-circuits the request; a function that fails
// (exception or abort) fails open and the chain continues into routing.
//
// The executor swaps a per-request EdgeFunctionRequestContext into the script
// context around each execution (restoring the previous services afterwards —
// production installs the HTTP upstream services there), so route-script
// execution later in the request is unaffected.
class EdgeFunctionExecutor final {
public:
    // Returns true when a function completed the response.
    [[nodiscard]] static fiber::async::Task<bool> run(std::span<const EdgeFunctionMounted> mounted,
                                                      http_script::ScriptExchangeCtx &script_context,
                                                      AccessRequestTelemetry &telemetry, std::string_view project,
                                                      std::string_view host) noexcept;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_EDGE_FUNCTION_EXECUTOR_H
