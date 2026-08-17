#ifndef FIBER_ACCESS_SERVER_ACCESS_REQUEST_HANDLER_H
#define FIBER_ACCESS_SERVER_ACCESS_REQUEST_HANDLER_H

#include <fiber/async/Task.h>
#include "../routing/AccessRouteSnapshot.h"
#include "AccessResult.h"
#include "ErrorResponder.h"
#include "ResponseExecutor.h"
#include "RoutePolicyEvaluator.h"
#include "TemplateEvaluator.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::http_script {
class ScriptExchangeCtx;
}

namespace fiber::access_server {

class AccessRequestTelemetry;
struct AccessRequestScriptAdapter {
    using ConditionFunction = bool (*)(void *context, http_script::ScriptExchangeCtx &script_context,
                                       const script::Script &program) noexcept;
    // Template callbacks append to output and must preserve bytes already
    // emitted by earlier literal/expression segments.
    using TemplateFunction = Result<void> (*)(void *context, http_script::ScriptExchangeCtx &script_context,
                                              const script::Script &program, std::string_view expression,
                                              std::string &output) noexcept;
    using RouteFunction = async::Task<Result<void>> (*)(void *context, http_script::ScriptExchangeCtx &script_context,
                                                        script::Script &program) noexcept;

    void *context = nullptr;
    ConditionFunction evaluate_condition = nullptr;
    TemplateFunction evaluate_template = nullptr;
    RouteFunction execute_route_script = nullptr;
};

struct AccessRequestHandlerOptions {
    // Matches fiber-net-gateway ServerConfig.DEF_MAX_BODY_SIZE.
    std::size_t default_max_request_body_size = 4U << 20U;
    ResponseExecutorOptions response{};
    bool test_mode = false;
};

struct ProxyExecutionInput {
    std::string_view call_source;
    std::string_view initial_context_cluster;
    std::string_view origin_host;
    TemplateEvaluator template_evaluator;
    std::size_t max_request_body_size = 0;
};

struct AccessProxyAdapter {
    using ExecuteFunction = async::Task<Result<void>> (*)(void *context, http::HttpExchange &exchange,
                                                          const CompiledProxyRoute &proxy, ProxyExecutionInput input,
                                                          AccessRequestTelemetry &telemetry) noexcept;

    void *context = nullptr;
    ExecuteFunction execute = nullptr;
};

class AccessRequestHandler {
public:
    AccessRequestHandler(AccessRouteSnapshotProvider snapshot_provider, AccessRequestScriptAdapter script_adapter = {},
                         AccessRequestHandlerOptions options = {}, AccessProxyAdapter proxy_adapter = {}) noexcept;

    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange,
                                           AccessRequestTelemetry &telemetry) const noexcept;

private:
    [[nodiscard]] async::Task<common::IoResult<void>>
    handle_and_finalize(http::HttpExchange &exchange, AccessRequestTelemetry &telemetry) const noexcept;
    [[nodiscard]] async::Task<Result<void>> handle_impl(http::HttpExchange &exchange,
                                                        AccessRequestTelemetry &telemetry) const noexcept;

    AccessRouteSnapshotProvider snapshot_provider_;
    AccessRequestScriptAdapter script_adapter_;
    AccessRequestHandlerOptions options_;
    AccessProxyAdapter proxy_adapter_;
    RoutePolicyEvaluator policy_evaluator_;
    ResponseExecutor response_executor_;
    ErrorResponder error_responder_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_REQUEST_HANDLER_H
