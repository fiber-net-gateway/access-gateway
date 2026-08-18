#include "ProxyExecutor.h"
#include "../observability/AccessRequestTelemetry.h"

#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/ClientHttp1Exchange.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/HttpBodyPipe.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/HttpProxyCore.h>
#include <fiber/http/HttpResponseWriter.h>
#include <fiber/http/HttpWebSocketProxy.h>
#include "ProxyRequestPlan.h"
#include "ProxyResponsePlan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceCluster = "HI-TRACE-CLUSTER";
constexpr std::size_t kMaxJavaAttempts = 4;

enum class ProxyFailurePhase : std::uint8_t {
    NoUpstreamHosts,
    UpstreamCircuitOpen,
    InvalidSelection,
    ResolveUpstream,
    PoolShutdown,
    Connect,
    Tls,
    BuildHeaders,
    SendHeader,
    ReadRequestBody,
    RequestBodyTooLarge,
    SendRequestBody,
    ReadResponseHeader,
    ReadResponseBody,
};

struct ProxyFailure {
    ProxyFailurePhase phase = ProxyFailurePhase::Connect;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
};

ProxyFailure failure(ProxyFailurePhase phase, const char *message,
                     common::IoErr io_error = common::IoErr::None) noexcept {
    return ProxyFailure{
            .phase = phase,
            .io_error = io_error,
            .message = message,
    };
}

ProxyFailure from_connect_error(const ProxyConnectError &connect_error) noexcept {
    ProxyFailurePhase phase = ProxyFailurePhase::Connect;
    switch (connect_error.code) {
        case ProxyConnectErrorCode::Resolve:
            phase = ProxyFailurePhase::ResolveUpstream;
            break;
        case ProxyConnectErrorCode::PoolShutdown:
            phase = ProxyFailurePhase::PoolShutdown;
            break;
        case ProxyConnectErrorCode::Connect:
            break;
        case ProxyConnectErrorCode::Tls:
            phase = ProxyFailurePhase::Tls;
            break;
    }
    return failure(phase, connect_error.message, connect_error.io_error);
}

ProxyFailure from_select_error(const ProxyAddressSelectError &select_error) noexcept {
    const ProxyFailurePhase phase = select_error.code == ProxyAddressSelectErrorCode::CircuitOpen
                                            ? ProxyFailurePhase::UpstreamCircuitOpen
                                            : ProxyFailurePhase::NoUpstreamHosts;
    return failure(phase, select_error.message, select_error.io_error);
}

AccessProxyFailurePhase metric_failure_phase(ProxyFailurePhase phase) noexcept {
    switch (phase) {
        case ProxyFailurePhase::NoUpstreamHosts:
            return AccessProxyFailurePhase::NoUpstreamHosts;
        case ProxyFailurePhase::UpstreamCircuitOpen:
            return AccessProxyFailurePhase::UpstreamCircuitOpen;
        case ProxyFailurePhase::InvalidSelection:
            return AccessProxyFailurePhase::InvalidSelection;
        case ProxyFailurePhase::ResolveUpstream:
            return AccessProxyFailurePhase::ResolveUpstream;
        case ProxyFailurePhase::PoolShutdown:
            return AccessProxyFailurePhase::PoolShutdown;
        case ProxyFailurePhase::Connect:
            return AccessProxyFailurePhase::Connect;
        case ProxyFailurePhase::Tls:
            return AccessProxyFailurePhase::Tls;
        case ProxyFailurePhase::BuildHeaders:
            return AccessProxyFailurePhase::BuildHeaders;
        case ProxyFailurePhase::SendHeader:
            return AccessProxyFailurePhase::SendHeader;
        case ProxyFailurePhase::ReadRequestBody:
            return AccessProxyFailurePhase::ReadRequestBody;
        case ProxyFailurePhase::RequestBodyTooLarge:
            return AccessProxyFailurePhase::RequestBodyTooLarge;
        case ProxyFailurePhase::SendRequestBody:
            return AccessProxyFailurePhase::SendRequestBody;
        case ProxyFailurePhase::ReadResponseHeader:
            return AccessProxyFailurePhase::ReadResponseHeader;
        case ProxyFailurePhase::ReadResponseBody:
            return AccessProxyFailurePhase::ReadResponseBody;
    }
    return AccessProxyFailurePhase::Connect;
}

std::string_view proxy_failure_phase_name(ProxyFailurePhase phase) noexcept {
    switch (phase) {
        case ProxyFailurePhase::NoUpstreamHosts:
            return "no_upstream_hosts";
        case ProxyFailurePhase::UpstreamCircuitOpen:
            return "upstream_circuit_open";
        case ProxyFailurePhase::InvalidSelection:
            return "invalid_selection";
        case ProxyFailurePhase::ResolveUpstream:
            return "resolve_upstream";
        case ProxyFailurePhase::PoolShutdown:
            return "pool_shutdown";
        case ProxyFailurePhase::Connect:
            return "connect";
        case ProxyFailurePhase::Tls:
            return "tls";
        case ProxyFailurePhase::BuildHeaders:
            return "build_headers";
        case ProxyFailurePhase::SendHeader:
            return "send_header";
        case ProxyFailurePhase::ReadRequestBody:
            return "read_request_body";
        case ProxyFailurePhase::RequestBodyTooLarge:
            return "request_body_too_large";
        case ProxyFailurePhase::SendRequestBody:
            return "send_request_body";
        case ProxyFailurePhase::ReadResponseHeader:
            return "read_response_header";
        case ProxyFailurePhase::ReadResponseBody:
            return "read_response_body";
    }
    return "unknown";
}

Exception http_client_error(std::uint32_t status, std::string_view name, std::string_view message) noexcept {
    return Exception{
            .name = name,
            .message = message,
            .status = status,
    };
}

Exception map_proxy_failure(const ProxyFailure &proxy_failure) noexcept {
    switch (proxy_failure.phase) {
        case ProxyFailurePhase::NoUpstreamHosts:
            return http_client_error(503, "UPSTREAM_NO_HOSTS",
                                     proxy_failure.message ? proxy_failure.message : "no upstream hosts are available");
        case ProxyFailurePhase::UpstreamCircuitOpen:
            return http_client_error(503, "UPSTREAM_CIRCUIT_BREAK",
                                     proxy_failure.message ? proxy_failure.message
                                                           : "upstream circuit breaker is open");
        case ProxyFailurePhase::InvalidSelection:
            return Exception::unknown(proxy_failure.message ? proxy_failure.message : "invalid upstream selection");
        case ProxyFailurePhase::ResolveUpstream:
            return http_client_error(503, "HTTP_CLIENT_DNS_ERROR", "dns resolve error");
        case ProxyFailurePhase::PoolShutdown:
        case ProxyFailurePhase::Connect:
            return http_client_error(502, "HTTP_CLIENT_CONNECT_ERROR", "cannot connect to upstream");
        case ProxyFailurePhase::Tls:
            return http_client_error(502, "HTTP_CLIENT_TLS_ERROR", "upstream TLS verification or negotiation failed");
        case ProxyFailurePhase::SendHeader:
        case ProxyFailurePhase::SendRequestBody:
            return http_client_error(502, "SEND_REQUEST_ERROR", "send request error");
        case ProxyFailurePhase::ReadResponseHeader:
            if (proxy_failure.io_error == common::IoErr::TimedOut) {
                return http_client_error(504, "REQUEST_TIMEOUT", "request timeout");
            }
            return http_client_error(502, "HTTP_NO_RESPONSE", "no response");
        case ProxyFailurePhase::ReadResponseBody:
            return http_client_error(502, "HTTP_RESPONSE_ABORT", "upstream response body aborted");
        case ProxyFailurePhase::RequestBodyTooLarge:
            return Exception::request_body_too_large();
        case ProxyFailurePhase::ReadRequestBody:
            return http_client_error(500, "HTTP_CLIENT_ERROR_BODY_SIZE",
                                     "predicated content length is not matched actual content length");
        case ProxyFailurePhase::BuildHeaders:
            return Exception::unknown(proxy_failure.message ? proxy_failure.message : "proxy request failed");
    }
    return Exception::unknown();
}

bool is_upstream_call_failure(ProxyFailurePhase phase) noexcept {
    switch (phase) {
        case ProxyFailurePhase::ResolveUpstream:
        case ProxyFailurePhase::PoolShutdown:
        case ProxyFailurePhase::Connect:
        case ProxyFailurePhase::Tls:
        case ProxyFailurePhase::SendHeader:
        case ProxyFailurePhase::SendRequestBody:
        case ProxyFailurePhase::ReadResponseHeader:
        case ProxyFailurePhase::ReadResponseBody:
            return true;
        case ProxyFailurePhase::NoUpstreamHosts:
        case ProxyFailurePhase::UpstreamCircuitOpen:
        case ProxyFailurePhase::InvalidSelection:
        case ProxyFailurePhase::BuildHeaders:
        case ProxyFailurePhase::ReadRequestBody:
        case ProxyFailurePhase::RequestBodyTooLarge:
            return false;
    }
    return false;
}

Result<void> proxy_failure_result(const ProxyFailure &proxy_failure) noexcept {
    if (proxy_failure.phase == ProxyFailurePhase::BuildHeaders && proxy_failure.io_error == common::IoErr::NoMem) {
        return std::unexpected(Err::from_error(common::IoErr::NoMem));
    }
    if (proxy_failure.phase == ProxyFailurePhase::ReadRequestBody && proxy_failure.io_error != common::IoErr::Invalid) {
        return std::unexpected(Err::from_error(proxy_failure.io_error));
    }
    const Exception exception = map_proxy_failure(proxy_failure);
    return std::unexpected(is_upstream_call_failure(proxy_failure.phase) ? Err::from_upstream_exception(exception)
                                                                         : Err::from_exception(exception));
}

class ProxyAttemptMetricScope final {
public:
    explicit ProxyAttemptMetricScope(AccessRequestTelemetry &telemetry) noexcept : telemetry_(&telemetry) {
        telemetry_->record_proxy_attempt_started();
    }

    ProxyAttemptMetricScope(const ProxyAttemptMetricScope &) = delete;
    ProxyAttemptMetricScope &operator=(const ProxyAttemptMetricScope &) = delete;
    ProxyAttemptMetricScope(ProxyAttemptMetricScope &&) = delete;
    ProxyAttemptMetricScope &operator=(ProxyAttemptMetricScope &&) = delete;

    ~ProxyAttemptMetricScope() noexcept { finish(AccessProxyAttemptResult::Aborted); }

    void completed() noexcept { finish(AccessProxyAttemptResult::Completed); }
    void failed() noexcept { finish(AccessProxyAttemptResult::Failed); }
    void aborted() noexcept { finish(AccessProxyAttemptResult::Aborted); }

private:
    void finish(AccessProxyAttemptResult result) noexcept {
        if (!telemetry_) {
            return;
        }
        telemetry_->record_proxy_attempt_finished(result);
        telemetry_ = nullptr;
    }

    AccessRequestTelemetry *telemetry_ = nullptr;
};

class WebSocketMetricScope final {
public:
    WebSocketMetricScope(AccessRequestTelemetry &telemetry, bool active) noexcept :
        telemetry_(&telemetry), active_(active) {}

    WebSocketMetricScope(const WebSocketMetricScope &) = delete;
    WebSocketMetricScope &operator=(const WebSocketMetricScope &) = delete;
    WebSocketMetricScope(WebSocketMetricScope &&) = delete;
    WebSocketMetricScope &operator=(WebSocketMetricScope &&) = delete;

    ~WebSocketMetricScope() noexcept {
        if (!active_) {
            return;
        }
        if (!handshake_finished_) {
            telemetry_->record_websocket_handshake(AccessWebSocketHandshakeResult::Failed);
        } else if (session_active_) {
            telemetry_->record_websocket_session_finished(AccessWebSocketSessionResult::Aborted);
        }
    }

    void rejected() noexcept {
        if (!active_ || handshake_finished_) {
            return;
        }
        telemetry_->record_websocket_handshake(AccessWebSocketHandshakeResult::Rejected);
        handshake_finished_ = true;
    }

    void accepted() noexcept {
        if (!active_ || handshake_finished_) {
            return;
        }
        telemetry_->record_websocket_handshake(AccessWebSocketHandshakeResult::Accepted);
        telemetry_->record_websocket_session_started();
        handshake_finished_ = true;
        session_active_ = true;
    }

    void closed() noexcept {
        if (!session_active_) {
            return;
        }
        telemetry_->record_websocket_session_finished(AccessWebSocketSessionResult::Closed);
        session_active_ = false;
    }

private:
    AccessRequestTelemetry *telemetry_ = nullptr;
    bool active_ = false;
    bool handshake_finished_ = false;
    bool session_active_ = false;
};

void record_provider_failure(AccessProviderTransaction &provider, AccessRequestTelemetry &telemetry,
                             ProxyAttemptMetricScope &attempt, const ProxyFailure &proxy_failure) noexcept {
    telemetry.record_proxy_failure(metric_failure_phase(proxy_failure.phase));
    if (is_upstream_call_failure(proxy_failure.phase)) {
        attempt.failed();
    } else {
        attempt.aborted();
    }
    const std::string_view phase = proxy_failure_phase_name(proxy_failure.phase);
    if (is_upstream_call_failure(proxy_failure.phase)) {
        provider.call_error(map_proxy_failure(proxy_failure), phase, proxy_failure.io_error);
    } else {
        provider.fail(phase, proxy_failure.io_error);
    }
}

Result<std::optional<std::string>> evaluate_proxy_context(std::span<const CompiledTemplateEntry> context,
                                                          TemplateEvaluator evaluator,
                                                          std::string_view initial_context_cluster,
                                                          AccessRequestTelemetry &telemetry) {
    std::optional<std::string> cluster;
    const std::optional<std::string_view> propagated_cluster = telemetry.trace_context(kTraceCluster);
    if (propagated_cluster && !propagated_cluster->empty()) {
        cluster.emplace(*propagated_cluster);
    } else if (!initial_context_cluster.empty()) {
        cluster.emplace(initial_context_cluster);
    }
    for (const CompiledTemplateEntry &entry: context) {
        auto value = evaluate_template(entry.value, evaluator);
        if (!value) {
            return std::unexpected(value.error());
        }
        const std::string_view value_view = value->view();
        if (value_view.empty()) {
            telemetry.remove_trace_context(entry.name);
        } else {
            auto stored = telemetry.put_trace_context(entry.name, value_view);
            if (!stored) {
                return std::unexpected(Err::from_error(stored.error()));
            }
        }
        if (entry.name == kTraceCluster) {
            if (value_view.empty()) {
                cluster.reset();
            } else {
                cluster = std::move(*value).into_string();
            }
        }
    }
    return cluster;
}

std::chrono::milliseconds response_header_timeout(std::int32_t timeout_millis) noexcept {
    if (timeout_millis < 0) {
        return std::chrono::milliseconds::max();
    }
    return std::chrono::milliseconds(timeout_millis);
}

ExceptionResult response_body_too_large(mem::BufPool &pool, std::size_t size) noexcept {
    std::array<char, std::numeric_limits<std::size_t>::digits10 + 1> buffer{};
    const auto formatted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), size);
    if (formatted.ec != std::errc{}) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return make_exception(pool, 500, "READ_RESP_BODY", "body size is too big：",
                          std::string_view(buffer.data(), static_cast<std::size_t>(formatted.ptr - buffer.data())));
}

bool parse_content_length(std::string_view value, std::size_t &output) noexcept {
    if (value.empty()) {
        return false;
    }
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return false;
    }
    output = result;
    return true;
}

bool build_downstream_headers(http::HttpExchange &downstream, const CompiledProxyRoute &proxy,
                              const ProxyUpstreamEndpoint &endpoint, const http::Http1ResponseHead &upstream_head,
                              std::span<const EvaluatedHeader> custom_headers, bool websocket_response,
                              const ClientMetadata &client_metadata, http::HttpHeaders &output) {
    for (const http::HttpHeaders::HeaderField &field: upstream_head.headers) {
        if (field.name_len == 0 || is_java_filtered_response_header(field.name_view()) ||
            proxy.response_headers.contains(field.lowcase_view(), field.name_hash)) {
            continue;
        }
        if (!output.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash)) {
            return false;
        }
    }
    for (const EvaluatedHeader &header: custom_headers) {
        if (!output.add_view(header.name, header.value.view(), header.lowcase_name.data(), header.name_hash)) {
            return false;
        }
    }

    if (!proxy.response_headers.contains("Location")) {
        const std::string_view location = upstream_head.headers.get("Location");
        if (!location.empty()) {
            auto rewritten = rewrite_java_proxy_location(location, endpoint.host_header,
                                                         client_metadata.external_scheme, downstream.header("Host"));
            if (rewritten && !output.set("Location", *rewritten)) {
                return false;
            }
        }
    }
    if (!proxy.response_headers.contains("Refresh")) {
        const std::string_view refresh = upstream_head.headers.get("Refresh");
        if (!refresh.empty()) {
            auto rewritten = rewrite_java_proxy_refresh(refresh, endpoint.host_header, client_metadata.external_scheme,
                                                        downstream.header("Host"));
            if (rewritten && !output.set("Refresh", *rewritten)) {
                return false;
            }
        }
    }

    if (websocket_response) {
        const std::string_view upgrade = upstream_head.headers.get("Upgrade");
        if (!output.set("Connection", "Upgrade") || !output.set("Upgrade", upgrade)) {
            return false;
        }
    } else {
        const std::string_view content_length = upstream_head.headers.get("Content-Length");
        if (!content_length.empty() && !output.set("Content-Length", content_length)) {
            return false;
        }
        if (proxy.flush.value_or(false) && !output.set("X-Accel-Buffering", "no")) {
            return false;
        }
    }
    return true;
}

bool response_limit_exceeded(const std::optional<std::uint64_t> &limit, std::size_t size) noexcept {
    return limit && *limit > 0 && size > *limit;
}

class ProxyResponseBodyReader {
public:
    ProxyResponseBodyReader(http::ClientHttp1Exchange &upstream,
                            std::optional<std::uint64_t> max_response_body_size) noexcept :
        upstream_(upstream), max_response_body_size_(max_response_body_size) {}

    async::Task<common::IoResult<mem::IoBufChain>> read_body(std::size_t max_bytes,
                                                             std::chrono::milliseconds timeout) noexcept {
        auto body = co_await upstream_.read_body(max_bytes, timeout);
        if (!body) {
            co_return std::unexpected(body.error());
        }

        const std::size_t body_size = body->readable_bytes();
        if (body_size > std::numeric_limits<std::size_t>::max() - received_body_) {
            response_limit_exceeded_ = true;
            co_return std::unexpected(common::IoErr::MessageTooLarge);
        }
        received_body_ += body_size;
        if (response_limit_exceeded(max_response_body_size_, received_body_)) {
            response_limit_exceeded_ = true;
            co_return std::unexpected(common::IoErr::MessageTooLarge);
        }
        co_return std::move(*body);
    }

    common::IoResult<void> abort(common::IoErr reason) noexcept { return upstream_.abort(reason); }

    [[nodiscard]] bool limit_exceeded() const noexcept { return response_limit_exceeded_; }

private:
    http::ClientHttp1Exchange &upstream_;
    std::optional<std::uint64_t> max_response_body_size_;
    std::size_t received_body_ = 0;
    bool response_limit_exceeded_ = false;
};

// Keep the connected attempt in this translation unit: Release builds can
// embed its directly-awaited Task frame in execute_impl instead of allocating
// another request-path coroutine.
class UpstreamAttempt final : public common::NonCopyable, public common::NonMovable {
public:
    UpstreamAttempt(http::HttpExchange &exchange, const CompiledProxyRoute &proxy, const ProxyExecutionInput &input,
                    AccessRequestTelemetry &telemetry, ProxyRequestPlan &request_plan, ProxyUpstreamEndpoint &endpoint,
                    ProxyUpstreamConnection &connection, ProxyAddressSelector &selector,
                    const ProxyExecutorOptions &options, AccessProviderTransaction &provider,
                    ProxyAttemptMetricScope &attempt_metrics, WebSocketMetricScope &websocket_metrics) noexcept :
        exchange_(exchange), proxy_(proxy), input_(input), telemetry_(telemetry), request_plan_(request_plan),
        endpoint_(endpoint), connection_(connection), selector_(selector), options_(options), provider_(provider),
        attempt_metrics_(attempt_metrics), websocket_metrics_(websocket_metrics) {}

    [[nodiscard]] async::Task<Result<void>> run() noexcept;

private:
    void report_selection(bool success) noexcept {
        if (selection_reported_) {
            return;
        }
        selector_.report_address(endpoint_, success);
        selection_reported_ = true;
    }

    http::HttpExchange &exchange_;
    const CompiledProxyRoute &proxy_;
    const ProxyExecutionInput &input_;
    AccessRequestTelemetry &telemetry_;
    ProxyRequestPlan &request_plan_;
    ProxyUpstreamEndpoint &endpoint_;
    ProxyUpstreamConnection &connection_;
    ProxyAddressSelector &selector_;
    const ProxyExecutorOptions &options_;
    AccessProviderTransaction &provider_;
    ProxyAttemptMetricScope &attempt_metrics_;
    WebSocketMetricScope &websocket_metrics_;
    bool selection_reported_ = false;
};

async::Task<Result<void>> UpstreamAttempt::run() noexcept {
    FIBER_ASSERT(connection_.connection != nullptr);
    provider_.add_connection_reuse(connection_.connection->request_count());
    telemetry_.set_upstream(endpoint_);

    http::HttpHeaders &request_headers = request_plan_.headers();
    if (!telemetry_.inject_upstream_headers(request_headers, provider_)) {
        telemetry_.record_proxy_failure(AccessProxyFailurePhase::BuildHeaders);
        attempt_metrics_.aborted();
        provider_.fail("build_headers", common::IoErr::NoMem);
        co_return std::unexpected(Err::from_error(common::IoErr::NoMem));
    }

    http::ClientHttp1Exchange upstream(*connection_.connection, exchange_.pool());
    if (!upstream.valid()) {
        const ProxyFailure exchange_failure =
                failure(ProxyFailurePhase::Connect, "failed to create upstream HTTP exchange", common::IoErr::Busy);
        record_provider_failure(provider_, telemetry_, attempt_metrics_, exchange_failure);
        co_return proxy_failure_result(exchange_failure);
    }

    const http::Http1RequestHead request_head = request_plan_.request_head(exchange_.method());
    auto sent_request_header = co_await upstream.send_header(request_head, request_plan_.request_end_stream());
    if (!sent_request_header) {
        const ProxyFailure send_failure = failure(
                ProxyFailurePhase::SendHeader, "failed to send upstream request header", sent_request_header.error());
        record_provider_failure(provider_, telemetry_, attempt_metrics_, send_failure);
        report_selection(false);
        co_return proxy_failure_result(send_failure);
    }

    const http::HttpBodySpec &request_body = request_plan_.body_spec();
    if (!request_plan_.request_end_stream()) {
        http::proxy_core::RequestBodyForwardState forward_state(request_body);
        std::size_t received_request_body = 0;
        for (;;) {
            auto body = co_await exchange_.read_body(options_.request_body_chunk_size);
            if (!body) {
                (void) upstream.abort(body.error());
                const ProxyFailure read_failure = failure(ProxyFailurePhase::ReadRequestBody,
                                                          "failed to read downstream request body", body.error());
                record_provider_failure(provider_, telemetry_, attempt_metrics_, read_failure);
                co_return proxy_failure_result(read_failure);
            }
            const bool complete = body->complete();
            const std::size_t body_bytes = body->readable_bytes();
            if (!forward_state.accepts(body_bytes)) {
                (void) upstream.abort(common::IoErr::Invalid);
                const ProxyFailure read_failure =
                        failure(ProxyFailurePhase::ReadRequestBody,
                                "downstream request body does not match Content-Length", common::IoErr::Invalid);
                record_provider_failure(provider_, telemetry_, attempt_metrics_, read_failure);
                co_return proxy_failure_result(read_failure);
            }
            if (input_.max_request_body_size != 0 &&
                body_bytes >
                        input_.max_request_body_size - std::min(received_request_body, input_.max_request_body_size)) {
                (void) upstream.abort(common::IoErr::MessageTooLarge);
                const ProxyFailure limit_failure =
                        failure(ProxyFailurePhase::RequestBodyTooLarge, "downstream request body exceeds route limit",
                                common::IoErr::MessageTooLarge);
                record_provider_failure(provider_, telemetry_, attempt_metrics_, limit_failure);
                co_return proxy_failure_result(limit_failure);
            }
            received_request_body += body_bytes;
            if (forward_state.should_write(body_bytes)) {
                auto written = co_await upstream.write_all(std::move(*body));
                if (!written) {
                    const ProxyFailure send_failure = failure(ProxyFailurePhase::SendRequestBody,
                                                              "failed to send upstream request body", written.error());
                    record_provider_failure(provider_, telemetry_, attempt_metrics_, send_failure);
                    report_selection(false);
                    co_return proxy_failure_result(send_failure);
                }
                if (*written != body_bytes) {
                    (void) upstream.abort(common::IoErr::Invalid);
                    const ProxyFailure send_failure =
                            failure(ProxyFailurePhase::SendRequestBody, "upstream request body write was incomplete",
                                    common::IoErr::Invalid);
                    record_provider_failure(provider_, telemetry_, attempt_metrics_, send_failure);
                    report_selection(false);
                    co_return proxy_failure_result(send_failure);
                }
                forward_state.record_write(*written);
            }
            if (complete) {
                if (request_body.is_content_length() && !forward_state.complete()) {
                    (void) upstream.abort(common::IoErr::Invalid);
                    const ProxyFailure read_failure =
                            failure(ProxyFailurePhase::ReadRequestBody,
                                    "downstream request body ended before Content-Length", common::IoErr::Invalid);
                    record_provider_failure(provider_, telemetry_, attempt_metrics_, read_failure);
                    co_return proxy_failure_result(read_failure);
                }
                break;
            }
        }
    }

    const http::Http1ResponseHead *upstream_head = nullptr;
    for (;;) {
        auto received = co_await upstream.read_header(response_header_timeout(proxy_.timeout_millis));
        if (!received) {
            const ProxyFailure response_failure = failure(ProxyFailurePhase::ReadResponseHeader,
                                                          "failed to read upstream response header", received.error());
            record_provider_failure(provider_, telemetry_, attempt_metrics_, response_failure);
            report_selection(false);
            co_return proxy_failure_result(response_failure);
        }
        if ((*received)->status_code == 101 || !(*received)->is_informational()) {
            upstream_head = *received;
            break;
        }
    }

    if (upstream_head->status_code >= 500) {
        report_selection(false);
    }
    const bool websocket_response = request_plan_.websocket_upgrade() && upstream_head->status_code == 101;
    if (request_plan_.websocket_upgrade() && !websocket_response) {
        websocket_metrics_.rejected();
    }

    auto custom_headers = prepare_proxy_response_headers(proxy_.response_headers, input_.template_evaluator);
    if (!custom_headers) {
        telemetry_.record_proxy_failure(AccessProxyFailurePhase::BuildResponseHeaders);
        if (websocket_response) {
            (void) upstream.abort(common::IoErr::Canceled);
            attempt_metrics_.aborted();
            provider_.fail("aborted", common::IoErr::Canceled);
        } else {
            auto discarded = co_await upstream.discard_response_body();
            if (!discarded) {
                report_selection(false);
                record_provider_failure(provider_, telemetry_, attempt_metrics_,
                                        failure(ProxyFailurePhase::ReadResponseBody,
                                                "failed to discard upstream response body", discarded.error()));
                (void) upstream.abort(discarded.error());
            } else {
                report_selection(true);
                attempt_metrics_.completed();
                provider_.complete(upstream_head->status_code);
            }
        }
        co_return std::unexpected(custom_headers.error());
    }

    const std::string_view content_length_text = upstream_head->headers.get("Content-Length");
    std::size_t content_length = 0;
    const bool has_content_length = parse_content_length(content_length_text, content_length);
    if (!websocket_response && has_content_length &&
        response_limit_exceeded(request_plan_.max_response_body_size(), content_length)) {
        (void) upstream.abort(common::IoErr::MessageTooLarge);
        telemetry_.record_proxy_failure(AccessProxyFailurePhase::ResponseBodyTooLarge);
        attempt_metrics_.aborted();
        provider_.fail("aborted", common::IoErr::MessageTooLarge);
        auto exception = response_body_too_large(exchange_.pool(), content_length);
        if (!exception) {
            co_return std::unexpected(Err::from_error(exception.error()));
        }
        co_return std::unexpected(Err::from_exception(*exception));
    }

    const bool no_body = http::proxy_core::response_has_no_body(exchange_.method(), upstream_head->status_code);
    if (websocket_response) {
        auto switched = upstream.switch_to_raw_stream();
        if (!switched) {
            report_selection(false);
            const Exception exception = Exception::unknown("failed to upgrade websocket");
            telemetry_.record_proxy_failure(AccessProxyFailurePhase::SwitchWebSocket);
            attempt_metrics_.failed();
            provider_.call_error(exception, "switch_to_raw_stream", switched.error());
            (void) upstream.abort(switched.error());
            co_return std::unexpected(Err::from_upstream_exception(exception));
        }
    }

    http::HttpHeaders &response_headers = telemetry_.response_headers();
    if (!build_downstream_headers(exchange_, proxy_, endpoint_, *upstream_head, *custom_headers, websocket_response,
                                  telemetry_.client_metadata(), response_headers) ||
        !telemetry_.finalize_response_headers()) {
        (void) upstream.abort(common::IoErr::NoMem);
        telemetry_.record_proxy_failure(AccessProxyFailurePhase::BuildResponseHeaders);
        attempt_metrics_.aborted();
        provider_.fail("aborted", common::IoErr::NoMem);
        co_return std::unexpected(Err::from_error(common::IoErr::NoMem));
    }

    if (websocket_response) {
        report_selection(true);

        auto sent_header = co_await telemetry_.response_writer().send_header(
                {
                        .kind = http::OutgoingHeaderKind::Final,
                        .status_code = 101,
                        .reason = upstream_head->reason,
                        .headers = &response_headers,
                        .body = http::HttpBodySpec::Stream(),
                        .connection_mode = http::ResponseConnectionMode::Auto,
                        .end_stream = false,
                },
                options_.downstream_write_timeout);
        if (!sent_header) {
            (void) upstream.abort(sent_header.error());
            telemetry_.record_proxy_failure(AccessProxyFailurePhase::SendResponseHeader);
            attempt_metrics_.aborted();
            provider_.fail("aborted", sent_header.error());
            telemetry_.record_response_error(sent_header.error());
            co_return std::unexpected(Err::from_error(sent_header.error()));
        }

        websocket_metrics_.accepted();
        const std::chrono::milliseconds websocket_timeout(request_plan_.websocket_timeout_millis());
        co_await http::proxy_core::relay_websocket_tunnel(exchange_, upstream, websocket_timeout, websocket_timeout);
        websocket_metrics_.closed();
        attempt_metrics_.completed();
        provider_.complete(upstream_head->status_code);
        co_return Result<void>{};
    }

    const http::HttpBodySpec response_body =
            no_body ? http::HttpBodySpec::None()
                    : (has_content_length ? http::HttpBodySpec::ContentLength(content_length)
                                          : http::HttpBodySpec::Auto());
    const bool response_end_stream = no_body || (has_content_length && content_length == 0);
    auto sent_response_header = co_await telemetry_.response_writer().send_header(
            {
                    .kind = http::OutgoingHeaderKind::Final,
                    .status_code = upstream_head->status_code,
                    .reason = upstream_head->reason,
                    .headers = &response_headers,
                    .body = response_body,
                    .connection_mode = http::ResponseConnectionMode::Auto,
                    .end_stream = response_end_stream,
            },
            options_.downstream_write_timeout);
    if (!sent_response_header) {
        (void) upstream.abort(sent_response_header.error());
        telemetry_.record_proxy_failure(AccessProxyFailurePhase::SendResponseHeader);
        attempt_metrics_.aborted();
        provider_.fail("aborted", sent_response_header.error());
        telemetry_.record_response_error(sent_response_header.error());
        co_return std::unexpected(Err::from_error(sent_response_header.error()));
    }
    if (response_end_stream) {
        if (no_body) {
            auto discarded = co_await upstream.discard_response_body();
            if (!discarded) {
                report_selection(false);
                record_provider_failure(provider_, telemetry_, attempt_metrics_,
                                        failure(ProxyFailurePhase::ReadResponseBody,
                                                "failed to discard upstream response body", discarded.error()));
                (void) upstream.abort(discarded.error());
            } else {
                report_selection(true);
                attempt_metrics_.completed();
                provider_.complete(upstream_head->status_code);
            }
        } else {
            report_selection(true);
            attempt_metrics_.completed();
            provider_.complete(upstream_head->status_code);
        }
        co_return Result<void>{};
    }

    ProxyResponseBodyReader body_reader(upstream, request_plan_.max_response_body_size());
    const http::HttpBodyPipeOptions pipe_options{
            .buffer_size = options_.response_body_chunk_size,
            .low_water = proxy_.flush.value_or(false)
                                 ? http::kUnbufferedBodyPipeLowWater
                                 : std::min(options_.response_body_chunk_size, http::kDefaultBodyPipeLowWater),
            .read_timeout = std::chrono::milliseconds::max(),
            .write_timeout = options_.downstream_write_timeout,
    };
    auto piped = co_await http::pipe_http_body(http::make_http_body_pipe_reader(body_reader),
                                               http::make_http_body_pipe_writer(telemetry_.response_writer()),
                                               event::EventLoop::current().io_buf_node_pool(), pipe_options);
    if (!piped) {
        const http::HttpBodyPipeError pipe_error = piped.error();
        if (body_reader.limit_exceeded()) {
            telemetry_.record_proxy_failure(AccessProxyFailurePhase::ResponseBodyTooLarge);
            attempt_metrics_.aborted();
            provider_.fail("aborted", pipe_error.code);
        } else if (pipe_error.phase == http::HttpBodyPipePhase::Read) {
            report_selection(false);
            record_provider_failure(provider_, telemetry_, attempt_metrics_,
                                    failure(ProxyFailurePhase::ReadResponseBody,
                                            "failed to read upstream response body", pipe_error.code));
        } else {
            telemetry_.record_proxy_failure(pipe_error.phase == http::HttpBodyPipePhase::Write
                                                    ? AccessProxyFailurePhase::WriteResponseBody
                                                    : AccessProxyFailurePhase::BuildResponseHeaders);
            attempt_metrics_.aborted();
            provider_.fail("aborted", pipe_error.code);
            if (pipe_error.phase == http::HttpBodyPipePhase::Write) {
                telemetry_.record_response_error(pipe_error.code);
            }
        }
        co_return std::unexpected(Err::from_error(pipe_error.code));
    }

    report_selection(true);
    attempt_metrics_.completed();
    provider_.complete(upstream_head->status_code);
    co_return Result<void>{};
}

} // namespace

ProxyExecutor::ProxyExecutor(http::StealableHttp1ConnectionPoolSet &pool, ProxyClusterMatcher cluster_matcher,
                             ProxyDnsResolver dns_resolver, ProxyExecutorOptions options) noexcept :
    pool_(pool), cluster_matcher_(cluster_matcher), dns_resolver_(dns_resolver), options_(options) {
    if (options_.request_body_chunk_size == 0) {
        options_.request_body_chunk_size = 64 * 1024;
    }
    if (options_.response_body_chunk_size == 0) {
        options_.response_body_chunk_size = 64 * 1024;
    }
}

AccessProxyAdapter ProxyExecutor::adapter() noexcept {
    return AccessProxyAdapter{
            .context = this,
            .execute = execute_adapter,
    };
}

async::Task<Result<void>> ProxyExecutor::execute_adapter(void *context, http::HttpExchange &exchange,
                                                         const CompiledProxyRoute &proxy, ProxyExecutionInput input,
                                                         AccessRequestTelemetry &telemetry) noexcept {
    return static_cast<ProxyExecutor *>(context)->execute(exchange, proxy, input, telemetry);
}

async::Task<Result<void>> ProxyExecutor::execute(http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                 ProxyExecutionInput input,
                                                 AccessRequestTelemetry &telemetry) noexcept {
    if (exchange.response_channel_closed()) {
        telemetry.record_proxy_execution(AccessProxyExecutionResult::Canceled);
        co_return Result<void>{};
    }

    auto completed =
            co_await async::when_any([&exchange]() { return exchange.wait_response_channel_closed(); },
                                     [&]() { return execute_impl(exchange, proxy, input, telemetry).select(); });
    if (completed.is<1>()) {
        auto result = std::move(completed).get<1>();
        telemetry.record_proxy_execution(result ? AccessProxyExecutionResult::Completed
                                                : AccessProxyExecutionResult::Failed);
        co_return result;
    }

    auto closed = std::move(completed).get<0>();
    if (!closed && !exchange.response_channel_closed()) {
        telemetry.record_proxy_execution(AccessProxyExecutionResult::Failed);
        co_return std::unexpected(Err::from_error(closed.error()));
    }
    telemetry.record_proxy_execution(AccessProxyExecutionResult::Canceled);
    co_return Result<void>{};
}

async::Task<Result<void>> ProxyExecutor::execute_impl(http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                      ProxyExecutionInput input,
                                                      AccessRequestTelemetry &telemetry) noexcept {
    FIBER_ASSERT(proxy.address_selector != nullptr);

    auto context_cluster =
            evaluate_proxy_context(proxy.context, input.template_evaluator, input.initial_context_cluster, telemetry);
    if (!context_cluster) {
        telemetry.record_proxy_failure(AccessProxyFailurePhase::EvaluateContext);
        co_return std::unexpected(context_cluster.error());
    }
    std::optional<std::string_view> cluster_override;
    if (*context_cluster) {
        cluster_override = **context_cluster;
    }
    if (cluster_matcher_.matches &&
        cluster_matcher_.matches(cluster_matcher_.context, exchange.header("X-Entry"), telemetry.client_metadata())) {
        cluster_override = std::string_view("gray");
        auto stored = telemetry.put_trace_context(kTraceCluster, "gray");
        if (!stored) {
            telemetry.record_proxy_failure(AccessProxyFailurePhase::EvaluateContext);
            co_return std::unexpected(Err::from_error(stored.error()));
        }
    }

    ProxyRequestPlan request_plan(exchange.pool(), exchange, proxy);
    WebSocketMetricScope websocket_metrics(telemetry, request_plan.websocket_upgrade());

    std::array<std::uint64_t, kMaxJavaAttempts> excluded_selection_tokens{};
    std::size_t excluded_selection_token_count = 0;
    std::optional<ProxyFailure> previous_failure;
    const UpstreamTlsTransportProfile *tls_profile = proxy.upstream_tls ? &*proxy.upstream_tls : nullptr;
    const UpstreamTlsClientPolicyView tls_policy =
            effective_upstream_tls_client_policy(options_.upstream_tls, tls_profile);

    for (std::size_t attempt = 0; attempt < kMaxJavaAttempts; ++attempt) {
        auto selected = proxy.address_selector->select_address(
                cluster_override,
                std::span<const std::uint64_t>(excluded_selection_tokens.data(), excluded_selection_token_count));
        if (!selected) {
            ProxyFailure selected_failure = previous_failure.value_or(from_select_error(selected.error()));
            if (!previous_failure) {
                telemetry.record_proxy_failure(metric_failure_phase(selected_failure.phase));
            }
            co_return proxy_failure_result(selected_failure);
        }
        if (selected->selection_token == 0) {
            const ProxyFailure selected_failure =
                    failure(ProxyFailurePhase::InvalidSelection,
                            "upstream selector returned an invalid selection token", common::IoErr::Invalid);
            telemetry.record_proxy_failure(metric_failure_phase(selected_failure.phase));
            co_return proxy_failure_result(selected_failure);
        }
        if (selected->connection_key == nullptr) {
            const ProxyFailure selected_failure =
                    failure(ProxyFailurePhase::InvalidSelection, "upstream cannot be used as a connection pool key",
                            common::IoErr::Invalid);
            telemetry.record_proxy_failure(metric_failure_phase(selected_failure.phase));
            co_return proxy_failure_result(selected_failure);
        }
        std::optional<http::Http1ConnectionGroupKey> profiled_connection_key;
        const http::Http1ConnectionGroupKey *connection_key = selected->connection_key;
        if (tls_profile) {
            profiled_connection_key = tls_profile->connection_key(*connection_key);
            if (!profiled_connection_key) {
                const ProxyFailure selected_failure =
                        failure(ProxyFailurePhase::InvalidSelection,
                                "upstream TLS profile cannot be applied to the connection key", common::IoErr::Invalid);
                telemetry.record_proxy_failure(metric_failure_phase(selected_failure.phase));
                co_return proxy_failure_result(selected_failure);
            }
            connection_key = &*profiled_connection_key;
        }
        ProxyRequestPlanResult planned = request_plan.prepared()
                                                 ? request_plan.rebind_endpoint(*selected)
                                                 : request_plan.prepare(*selected, exchange, proxy, input, telemetry);
        if (!planned) {
            telemetry.record_proxy_failure(planned.error().phase == ProxyRequestPlanErrorPhase::BuildRequest
                                                   ? AccessProxyFailurePhase::BuildRequest
                                                   : AccessProxyFailurePhase::BuildHeaders);
            co_return std::unexpected(planned.error().error);
        }

        const std::string_view provider_name =
                selected->provider_name.empty() ? selected->host_header : selected->provider_name;
        AccessProviderTransaction provider_transaction = telemetry.start_provider_transaction(provider_name);
        provider_transaction.add_upstream(selected->host_header, attempt + 1);
        ProxyAttemptMetricScope attempt_metrics(telemetry);

        auto connected = co_await acquire_proxy_upstream_connection(pool_, dns_resolver_, *connection_key, tls_policy,
                                                                    options_.connect_timeout, options_.happy_eyeballs);
        if (!connected) {
            telemetry.record_proxy_connection(connected.error().observation);
            ProxyFailure connect_failure = from_connect_error(connected.error());
            record_provider_failure(provider_transaction, telemetry, attempt_metrics, connect_failure);
            proxy.address_selector->report_address(*selected, false);
            FIBER_ASSERT(excluded_selection_token_count < excluded_selection_tokens.size());
            excluded_selection_tokens[excluded_selection_token_count++] = selected->selection_token;
            previous_failure = connect_failure;
            continue;
        }
        telemetry.record_proxy_connection(connected->observation);
        UpstreamAttempt upstream_attempt(exchange, proxy, input, telemetry, request_plan, *selected, *connected,
                                         *proxy.address_selector, options_, provider_transaction, attempt_metrics,
                                         websocket_metrics);
        co_return co_await upstream_attempt.run();
    }

    if (!previous_failure) {
        telemetry.record_proxy_failure(AccessProxyFailurePhase::NoUpstreamHosts);
    }
    co_return proxy_failure_result(previous_failure.value_or(
            failure(ProxyFailurePhase::NoUpstreamHosts, "no usable upstream", common::IoErr::NotFound)));
}

} // namespace fiber::access_server
