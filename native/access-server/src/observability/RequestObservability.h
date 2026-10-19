#ifndef FIBER_ACCESS_SERVER_REQUEST_OBSERVABILITY_H
#define FIBER_ACCESS_SERVER_REQUEST_OBSERVABILITY_H

#include "AccessProviderTransaction.h"
#include "AccessServerMetrics.h"

#include <chrono>
#include <optional>
#include <string_view>

#include <fiber/cat/Transaction.h>
#include <fiber/common/IoError.h>

namespace fiber::cat {
class CatClient;
}

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

struct ClientMetadata;
struct CompiledRoute;
struct Exception;
struct ProxyConnectionObservation;
struct ProxyUpstreamEndpoint;
class ScriptExecutionContext;
class TracePropagation;

// Java ClusteredLoadBalancer.loadBalance records one LoadBalance event per
// address-selection attempt; the outcome maps to the Java statuses SUC / "cbt"
// / ERROR.
enum class AccessClusterSelectionResult : std::uint8_t {
    Selected,
    CircuitBroken,
    NoHosts,
};

// One address-selection attempt: the request-specified cluster (trace context
// or gray), the route's configured cluster, and whether the decider fell back
// to the configured cluster after the request-specified selection failed.
struct AccessClusterSelection {
    std::string_view service;
    std::optional<std::string_view> requested_cluster;
    std::optional<std::string_view> configured_cluster;
    bool fell_back = false;
    AccessClusterSelectionResult result = AccessClusterSelectionResult::Selected;
};

// Owns request metrics, CAT records, bounded log fields, and terminal request
// accounting. Request execution and trace propagation remain explicit inputs.
class RequestObservability final {
public:
    RequestObservability(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics, cat::CatClient *cat_client,
                         const ClientMetadata &client_metadata, TracePropagation &trace) noexcept;

    RequestObservability(const RequestObservability &) = delete;
    RequestObservability &operator=(const RequestObservability &) = delete;
    RequestObservability(RequestObservability &&) = delete;
    RequestObservability &operator=(RequestObservability &&) = delete;

    void finish(http::HttpExchange &exchange) noexcept;

    void set_project(ScriptExecutionContext &execution, std::string_view project, std::string_view effective_host,
                     std::string_view context_cluster, std::string_view host_pattern) noexcept;
    void set_route(ScriptExecutionContext &execution, const CompiledRoute &route) noexcept;
    void record_exception(ScriptExecutionContext &execution, const Exception &exception) noexcept;
    void record_upstream_exception(ScriptExecutionContext &execution, const Exception &exception) noexcept;
    // Failure mark only; the retired root-level ResponseError event is
    // superseded by the stage-attributed CALL_ERROR / ReqBodyErr / RespBodyErr
    // events on the provider transaction.
    void record_response_error(ScriptExecutionContext &execution, common::IoErr error) noexcept;
    // The error responder itself failing to deliver its page: a root-level
    // CALL_ERROR/write_resp event with the error_responder phase.
    void record_error_response_failure(ScriptExecutionContext &execution, common::IoErr error) noexcept;
    void record_response_compression(bool compressed) noexcept;
    void record_response_compression_not_acceptable() noexcept;
    void record_proxy_execution(AccessProxyExecutionResult result) noexcept;
    void record_proxy_attempt_started() noexcept;
    void record_proxy_attempt_finished(AccessProxyAttemptResult result) noexcept;
    void record_proxy_failure(AccessProxyFailurePhase phase) noexcept;
    void record_proxy_connection(const ProxyConnectionObservation &observation) noexcept;
    void record_websocket_handshake(AccessWebSocketHandshakeResult result) noexcept;
    void record_websocket_session_started() noexcept;
    void record_websocket_session_finished(AccessWebSocketSessionResult result) noexcept;
    void record_proxy_body_idle_timeout(AccessBodyIdleDirection direction) noexcept;
    // Dynamic-series recorders. The response metric fires at upstream
    // response-head arrival (per attempt); the transport-error label is the
    // coarse connect/request_timeout/io_err classification computed by the
    // proxy executor; circuit rejections fire per failed address selection.
    void record_provider_response(const ProxyUpstreamEndpoint &endpoint, int status_code) noexcept;
    void record_proxy_transport_error(std::string_view app, std::string_view ip_port,
                                      std::string_view error_label) noexcept;
    void record_circuit_rejection(std::string_view app) noexcept;
    void mark_io_error(ScriptExecutionContext &execution, common::IoErr error) noexcept;
    void set_upstream(ScriptExecutionContext &execution, const ProxyUpstreamEndpoint &endpoint) noexcept;
    void record_cluster_selection(const AccessClusterSelection &selection) noexcept;
    [[nodiscard]] AccessProviderTransaction start_provider_transaction(std::string_view name) noexcept;
    void start_websocket_session(std::string_view name, bool extended_connect) noexcept;
    void finish_websocket_session(bool closed) noexcept;

    [[nodiscard]] cat::Transaction &root_transaction() noexcept { return root_; }

private:
    void add_root_data(std::string_view key, std::string_view value) noexcept;
    void mark_failed(ScriptExecutionContext &execution, std::string_view error) noexcept;
    void update_transaction_name(ScriptExecutionContext &execution) noexcept;

    AccessServerMetrics::Worker *metrics_ = nullptr;
    std::chrono::steady_clock::time_point started_{};
    cat::Transaction root_;
    cat::Event websocket_event_;
    std::chrono::steady_clock::time_point websocket_started_{};
    std::string_view project_;
    std::string_view host_pattern_;
    std::string_view route_;
    std::string_view upstream_;
    std::string_view response_compression_;
    std::string_view error_;
    bool execution_failed_ = false;
    bool failure_recorded_ = false;
    bool exception_recorded_ = false;
    bool finished_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_REQUEST_OBSERVABILITY_H
