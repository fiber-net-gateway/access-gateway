#include "EdgeFunctionExecutor.h"

#include "../observability/AccessEdgeFunctionTransaction.h"
#include "../observability/AccessRequestTelemetry.h"
#include "../observability/AccessServerLogCategories.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <fiber/common/util/Crc32.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/log/Log.h>
#include <fiber/script/ScriptResult.h>

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_EDGE_FUNCTION, kAccessServerEdgeFunctionLogger);

// Fixed reason vocabulary for access_server_function_errors_total.
constexpr std::string_view kErrorReasonException = "exception";
constexpr std::string_view kErrorReasonAbort = "abort";

// Lowercased ScriptResultKind name for the per-execution CAT transaction's
// "result" data tag — the executor's own result classification, verbatim.
constexpr std::string_view script_result_kind_name(script::ScriptResultKind kind) noexcept {
    switch (kind) {
        case script::ScriptResultKind::Value:
            return "value";
        case script::ScriptResultKind::Void:
            return "void";
        case script::ScriptResultKind::Exception:
            return "exception";
        case script::ScriptResultKind::Abort:
            return "abort";
    }
    return {};
}

// Sticky sampling bucket (design §3.1 挂载采样): CRC-32 over the client IP
// bytes (v4 or v6, branch-free), so the same client lands in the same bucket
// on every request and on every instance. The resolved client address is what
// makes that true behind a CDN or load balancer, where the socket peer is an
// intermediary; it falls back to the peer when none resolved. Computed at most
// once per request and shared by all sampled functions, which keeps cohorts
// monotone: raising a ratio only adds clients, never swaps them.
std::uint32_t sampling_bucket(const ClientMetadata &client) noexcept {
    const net::IpAddress &address = client.has_client_address ? client.client_address : client.peer_address;
    return fiber::util::Crc32::compute(std::span<const std::uint8_t>{address.data(), address.byte_size()}) %
           static_cast<std::uint32_t>(kEdgeFunctionFullRatio);
}

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

fiber::async::Task<EdgeFunctionOutcome>
EdgeFunctionExecutor::run(std::span<const EdgeFunctionMounted> mounted, http_script::ScriptExchangeCtx &script_context,
                          AccessRequestTelemetry &telemetry, std::string_view project, std::string_view host,
                          EdgeFunctionPhase phase, bool fail_close, std::string_view request_body) noexcept {
    if (mounted.empty()) {
        co_return EdgeFunctionOutcome::Continue;
    }
    RequestEdgeContext edge(telemetry);
    edge.project = project;
    edge.host = host;
    edge.request_body = request_body;
    // Production installs the HTTP upstream services for the request's route
    // scripts; edge functions must see (and leave) the original pointer.
    http_script::HttpScriptServices *const previous_services = script_context.services();
    // Sampling happens before execution (design §3.2): a skipped function
    // leaves no GcHeap entry and only bumps the sampled_out counter. The draw
    // is lazy — full-ratio requests never compute it.
    std::optional<std::uint32_t> bucket;
    for (const EdgeFunctionMounted &entry: mounted) {
        if (!entry.always_runs()) {
            if (entry.never_runs()) {
                // Mounted but disabled, or fully taken over by a canary:
                // subscription and evidence stay live, execution stays off.
                telemetry.record_edge_function_sampled_out(entry.name, project);
                continue;
            }
            if (!bucket.has_value()) {
                bucket = sampling_bucket(telemetry.client_metadata());
            }
            if (*bucket < static_cast<std::uint32_t>(entry.bucket_begin) ||
                *bucket >= static_cast<std::uint32_t>(entry.bucket_end)) {
                telemetry.record_edge_function_sampled_out(entry.name, project);
                continue;
            }
        }
        edge.begin_function(entry.name);
        script_context.set_services(&edge);
        // Per-execution CAT transaction (a no-op without a CAT root): the
        // script heap is quiescent inside this window — only this function
        // allocates on it — so lifetime-counter deltas attribute cleanly.
        AccessEdgeFunctionTransaction transaction = telemetry.start_edge_function_transaction(entry.name);
        transaction.add_phase(phase);
        const script::GcHeapStats heap_before = script_context.heap().stats();
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        script::JsValue root = script::JsValue::make_undefined();
        const script::ScriptResult result =
                co_await entry.program->program.exec_async(root, &script_context, script_context.heap());
        const std::chrono::nanoseconds elapsed = std::chrono::steady_clock::now() - started;
        script_context.set_services(previous_services);
        transaction.add_cost(heap_before, script_context.heap().stats());

        telemetry.record_edge_function_evaluated(entry.name, project);
        telemetry.record_edge_function_duration(entry.name, elapsed);
        if (script_context.response_header_sent()) {
            transaction.complete_success(script_result_kind_name(result.kind));
            LOG(LOG_EDGE_FUNCTION, INFO) << "edge_function_short_circuit"
                                         << " trace_id=\"" << telemetry.trace_id() << "\"" << " project=\"" << project
                                         << "\"" << " host=\"" << host << "\"" << " function=\"" << entry.name << "\"";
            co_return EdgeFunctionOutcome::Responded;
        }
        switch (result.kind) {
            case script::ScriptResultKind::Void:
            case script::ScriptResultKind::Value:
                // Completed without writing a response: continue the chain.
                transaction.complete_success(script_result_kind_name(result.kind));
                break;
            case script::ScriptResultKind::Exception:
                transaction.complete_error(script_result_kind_name(result.kind), kErrorReasonException);
                telemetry.record_edge_function_error(entry.name, kErrorReasonException);
                LOG(LOG_EDGE_FUNCTION, WARN)
                        << "edge_function_error"
                        << " trace_id=\"" << telemetry.trace_id() << "\"" << " project=\"" << project << "\""
                        << " function=\"" << entry.name << "\"" << " reason=\"" << kErrorReasonException << "\"";
                if (fail_close) {
                    co_return EdgeFunctionOutcome::Rejected;
                }
                break;
            case script::ScriptResultKind::Abort:
                transaction.complete_error(script_result_kind_name(result.kind), kErrorReasonAbort,
                                           script::abort_reason_name(result.abort().reason));
                telemetry.record_edge_function_error(entry.name, kErrorReasonAbort);
                LOG(LOG_EDGE_FUNCTION, WARN)
                        << "edge_function_error"
                        << " trace_id=\"" << telemetry.trace_id() << "\"" << " project=\"" << project << "\""
                        << " function=\"" << entry.name << "\"" << " reason=\"" << kErrorReasonAbort << "\" detail=\""
                        << script::abort_reason_name(result.abort().reason) << "\"";
                if (fail_close) {
                    co_return EdgeFunctionOutcome::Rejected;
                }
                break;
        }
    }
    co_return EdgeFunctionOutcome::Continue;
}

} // namespace fiber::access_server
