#include "EdgeFunctionExecutor.h"

#include "../observability/AccessRequestTelemetry.h"
#include "../observability/AccessServerLogCategories.h"

#include <chrono>
#include <cstddef>
#include <string_view>

#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/log/Log.h>
#include <fiber/script/ScriptResult.h>

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_EDGE_FUNCTION, kAccessServerEdgeFunctionLogger);

// Fixed reason vocabulary for access_server_function_errors_total.
constexpr std::string_view kErrorReasonException = "exception";
constexpr std::string_view kErrorReasonAbort = "abort";

// Per-request edge context: backs ctx.report and the $ctx constants. The
// report budget is per function per execution, so begin_function() resets it
// for every mounted function on the chain.
class RequestEdgeContext final : public EdgeFunctionRequestContext {
public:
    explicit RequestEdgeContext(AccessRequestTelemetry &telemetry) noexcept : telemetry_(&telemetry) {}

    void report(std::string_view event, std::string_view label) noexcept override {
        if (overflow_recorded_) {
            return; // already over budget for this function; stay silent
        }
        if (reports_ >= kAccessConfigLimits.edge_functions.max_reports_per_execution) {
            overflow_recorded_ = true;
            telemetry_->record_edge_function_report_overflow(function);
            return;
        }
        ++reports_;
        telemetry_->record_edge_function_event(function, event, label);
        LOG(LOG_EDGE_FUNCTION, INFO) << "edge_function_report"
                                     << " trace_id=\"" << telemetry_->trace_id() << "\"" << " project=\"" << project
                                     << "\"" << " host=\"" << host << "\"" << " function=\"" << function << "\""
                                     << " event=\"" << event << "\"" << " label=\"" << label << "\"";
    }

    void begin_function(std::string_view name) noexcept {
        function = name;
        reports_ = 0;
        overflow_recorded_ = false;
    }

private:
    AccessRequestTelemetry *telemetry_;
    std::size_t reports_ = 0;
    bool overflow_recorded_ = false;
};

} // namespace

fiber::async::Task<bool> EdgeFunctionExecutor::run(std::span<const EdgeFunctionMounted> mounted,
                                                   http_script::ScriptExchangeCtx &script_context,
                                                   AccessRequestTelemetry &telemetry, std::string_view project,
                                                   std::string_view host) noexcept {
    if (mounted.empty()) {
        co_return false;
    }
    RequestEdgeContext edge(telemetry);
    edge.project = project;
    edge.host = host;
    // Production installs the HTTP upstream services for the request's route
    // scripts; edge functions must see (and leave) the original pointer.
    http_script::HttpScriptServices *const previous_services = script_context.services();
    for (const EdgeFunctionMounted &entry: mounted) {
        edge.begin_function(entry.name);
        script_context.set_services(&edge);
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        script::JsValue root = script::JsValue::make_undefined();
        const script::ScriptResult result =
                co_await entry.program->program.exec_async(root, &script_context, script_context.heap());
        const std::chrono::nanoseconds elapsed = std::chrono::steady_clock::now() - started;
        script_context.set_services(previous_services);

        telemetry.record_edge_function_evaluated(entry.name, project);
        telemetry.record_edge_function_duration(entry.name, elapsed);
        if (script_context.response_header_sent()) {
            LOG(LOG_EDGE_FUNCTION, INFO) << "edge_function_short_circuit"
                                         << " trace_id=\"" << telemetry.trace_id() << "\"" << " project=\"" << project
                                         << "\"" << " host=\"" << host << "\"" << " function=\"" << entry.name << "\"";
            co_return true;
        }
        switch (result.kind) {
            case script::ScriptResultKind::Void:
            case script::ScriptResultKind::Value:
                // Completed without writing a response: continue the chain.
                break;
            case script::ScriptResultKind::Exception:
                telemetry.record_edge_function_error(entry.name, kErrorReasonException);
                LOG(LOG_EDGE_FUNCTION, WARN)
                        << "edge_function_error"
                        << " trace_id=\"" << telemetry.trace_id() << "\"" << " project=\"" << project << "\""
                        << " function=\"" << entry.name << "\"" << " reason=\"" << kErrorReasonException << "\"";
                break;
            case script::ScriptResultKind::Abort:
                telemetry.record_edge_function_error(entry.name, kErrorReasonAbort);
                LOG(LOG_EDGE_FUNCTION, WARN)
                        << "edge_function_error"
                        << " trace_id=\"" << telemetry.trace_id() << "\"" << " project=\"" << project << "\""
                        << " function=\"" << entry.name << "\"" << " reason=\"" << kErrorReasonAbort << "\" detail=\""
                        << script::abort_reason_name(result.abort().reason) << "\"";
                break;
        }
    }
    co_return false;
}

} // namespace fiber::access_server
