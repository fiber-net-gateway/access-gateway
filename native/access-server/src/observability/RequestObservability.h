#ifndef FIBER_ACCESS_SERVER_REQUEST_OBSERVABILITY_H
#define FIBER_ACCESS_SERVER_REQUEST_OBSERVABILITY_H

#include "AccessProviderTransaction.h"
#include "AccessServerMetrics.h"

#include <chrono>
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
class AccessLogPolicy;
class ScriptExecutionContext;
class TracePropagation;

// Owns request metrics, CAT records, bounded log fields, and terminal request
// accounting. Request execution and trace propagation remain explicit inputs.
class RequestObservability final {
public:
    RequestObservability(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics, cat::CatClient *cat_client,
                         const AccessLogPolicy *access_log_policy, const ClientMetadata &client_metadata,
                         TracePropagation &trace) noexcept;

    RequestObservability(const RequestObservability &) = delete;
    RequestObservability &operator=(const RequestObservability &) = delete;
    RequestObservability(RequestObservability &&) = delete;
    RequestObservability &operator=(RequestObservability &&) = delete;

    void finish(http::HttpExchange &exchange, const ClientMetadata &client_metadata,
                std::string_view trace_id) noexcept;

    void set_project(ScriptExecutionContext &execution, std::string_view project, std::string_view effective_host,
                     std::string_view context_cluster) noexcept;
    void set_route(ScriptExecutionContext &execution, const CompiledRoute &route) noexcept;
    void record_exception(ScriptExecutionContext &execution, const Exception &exception) noexcept;
    void record_upstream_exception(ScriptExecutionContext &execution, const Exception &exception) noexcept;
    void record_response_error(ScriptExecutionContext &execution, common::IoErr error) noexcept;
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
    void mark_io_error(ScriptExecutionContext &execution, common::IoErr error) noexcept;
    void set_upstream(ScriptExecutionContext &execution, const ProxyUpstreamEndpoint &endpoint) noexcept;
    [[nodiscard]] AccessProviderTransaction start_provider_transaction(std::string_view name) noexcept;

    [[nodiscard]] cat::Transaction &root_transaction() noexcept { return root_; }

private:
    void add_root_data(std::string_view key, std::string_view value) noexcept;
    void mark_failed(ScriptExecutionContext &execution, std::string_view error) noexcept;
    void update_transaction_name(ScriptExecutionContext &execution) noexcept;

    AccessServerMetrics::Worker *metrics_ = nullptr;
    const AccessLogPolicy *access_log_policy_ = nullptr;
    std::chrono::steady_clock::time_point started_{};
    cat::Transaction root_;
    std::string_view project_;
    std::string_view route_;
    std::string_view cluster_;
    std::string_view upstream_;
    std::string_view response_compression_;
    std::string_view error_;
    bool execution_failed_ = false;
    bool failure_recorded_ = false;
    bool exception_recorded_ = false;
    bool response_error_recorded_ = false;
    bool finished_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_REQUEST_OBSERVABILITY_H
