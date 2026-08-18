#ifndef FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H
#define FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H

#include "../execution/ClientMetadata.h"
#include "AccessProviderTransaction.h"
#include "RequestObservability.h"
#include "ScriptExecutionContext.h"
#include "TracePropagation.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/http/GzipResponseWriter.h>
#include <fiber/http/HttpResponseWriter.h>

namespace fiber::cat {
class CatClient;
}

namespace fiber::http {
class HttpExchange;
}

namespace fiber::http_script {
class ConstPackage;
}

namespace fiber::access_server {

struct Exception;
struct CompiledRoute;
struct ProxyUpstreamEndpoint;
struct ProxyConnectionObservation;
class AccessLogPolicy;
class ClientMetadataResolver;

// Stable request-lifetime façade. By-value components keep request ownership in
// one place while separating scripting, propagation, and observability policy.
class AccessRequestTelemetry final : public common::NonCopyable, public common::NonMovable {
public:
    AccessRequestTelemetry(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                           cat::CatClient *cat_client, const AccessLogPolicy *access_log_policy = nullptr,
                           const ClientMetadataResolver *client_metadata_resolver = nullptr) noexcept;
    ~AccessRequestTelemetry() noexcept;

    void set_project(std::string_view project, std::string_view effective_host,
                     std::string_view context_cluster) noexcept;
    void set_route(const CompiledRoute &route) noexcept { observability_.set_route(execution_, route); }
    void record_exception(const Exception &exception) noexcept {
        observability_.record_exception(execution_, exception);
    }
    void record_upstream_exception(const Exception &exception) noexcept {
        observability_.record_upstream_exception(execution_, exception);
    }
    void record_response_error(common::IoErr error) noexcept {
        observability_.record_response_error(execution_, error);
    }
    void record_response_compression(bool compressed) noexcept {
        observability_.record_response_compression(compressed);
        response_compression_recorded_ = true;
    }
    void record_response_compression_not_acceptable() noexcept {
        observability_.record_response_compression_not_acceptable();
        response_compression_recorded_ = true;
    }
    void record_proxy_execution(AccessProxyExecutionResult result) noexcept {
        observability_.record_proxy_execution(result);
    }
    void record_proxy_attempt_started() noexcept { observability_.record_proxy_attempt_started(); }
    void record_proxy_attempt_finished(AccessProxyAttemptResult result) noexcept {
        observability_.record_proxy_attempt_finished(result);
    }
    void record_proxy_failure(AccessProxyFailurePhase phase) noexcept { observability_.record_proxy_failure(phase); }
    void record_proxy_connection(const ProxyConnectionObservation &observation) noexcept {
        observability_.record_proxy_connection(observation);
    }
    void record_websocket_handshake(AccessWebSocketHandshakeResult result) noexcept {
        observability_.record_websocket_handshake(result);
    }
    void record_websocket_session_started() noexcept { observability_.record_websocket_session_started(); }
    void record_websocket_session_finished(AccessWebSocketSessionResult result) noexcept {
        observability_.record_websocket_session_finished(result);
    }
    void mark_io_error(common::IoErr error) noexcept { observability_.mark_io_error(execution_, error); }
    void set_upstream(const ProxyUpstreamEndpoint &endpoint) noexcept {
        observability_.set_upstream(execution_, endpoint);
    }
    [[nodiscard]] AccessProviderTransaction start_provider_transaction(std::string_view name) noexcept {
        return observability_.start_provider_transaction(name);
    }

    [[nodiscard]] std::string_view trace_id() const noexcept { return trace_.trace_id(); }
    [[nodiscard]] std::string_view trace_parent() const noexcept { return trace_.trace_parent(); }
    [[nodiscard]] std::optional<std::string_view> trace_context(std::string_view key) const noexcept {
        return trace_.trace_context(key);
    }
    [[nodiscard]] common::IoResult<void> bind_trace_context(const http_script::ConstPackage &constants) noexcept {
        return trace_.bind_trace_context(execution_, constants);
    }
    [[nodiscard]] common::IoResult<void> put_trace_context(std::string_view key, std::string_view value) noexcept {
        return trace_.put_trace_context(execution_, observability_.root_transaction(), key, value);
    }
    void remove_trace_context(std::string_view key) noexcept {
        trace_.remove_trace_context(execution_, observability_.root_transaction(), key);
    }
    [[nodiscard]] http_script::ScriptExchangeCtx &script_context() noexcept { return execution_.script_context(); }
    [[nodiscard]] const http_script::ScriptExchangeCtx &script_context() const noexcept {
        return execution_.script_context();
    }
    [[nodiscard]] http::HttpHeaders &response_headers() noexcept { return execution_.response_headers(); }
    [[nodiscard]] const http::HttpHeaders &response_headers() const noexcept { return execution_.response_headers(); }
    [[nodiscard]] http::HttpResponseWriter &response_writer() noexcept { return response_writer_; }
    [[nodiscard]] const http::HttpResponseWriter &response_writer() const noexcept { return response_writer_; }
    [[nodiscard]] common::IoResult<void> enable_response_compression(std::uint8_t level,
                                                                     bool request_accepts_gzip) noexcept;
    [[nodiscard]] const ClientMetadata &client_metadata() const noexcept { return client_metadata_; }
    [[nodiscard]] bool finalize_response_headers() noexcept {
        return trace_.finalize_response_headers(execution_.response_headers());
    }
    [[nodiscard]] bool inject_upstream_headers(http::HttpHeaders &headers,
                                               AccessProviderTransaction &provider) noexcept {
        return trace_.inject_upstream_headers(headers, execution_, observability_.root_transaction(), provider);
    }

private:
    // Construction order is a lifetime invariant: observability is destroyed
    // before the trace, metadata, and exchange-backed execution state it reads.
    ScriptExecutionContext execution_;
    http::HttpResponseWriter response_writer_;
    std::optional<http::GzipResponseWriter> gzip_writer_;
    bool response_compression_recorded_ = false;
    ClientMetadata client_metadata_;
    TracePropagation trace_;
    RequestObservability observability_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H
