#include "RequestObservability.h"

#include "AccessLogPolicy.h"
#include "AccessServerLogCategories.h"
#include "ScriptExecutionContext.h"
#include "TracePropagation.h"

#include "../execution/AccessResult.h"
#include "../execution/ClientMetadata.h"
#include "../execution/ProxyUpstreamConnection.h"
#include "../routing/ProjectRouteSnapshot.h"
#include "../routing/ProxyAddressSelector.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <fiber/cat/CatClient.h>
#include <fiber/cat/MessageTrace.h>
#include <fiber/cat/Status.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/log/Log.h>

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_ACCESS, kAccessServerAccessLogger);

constexpr std::string_view kTraceIdLowcaseHeader = "hi-trace-id";
constexpr std::uint64_t kTraceIdHeaderHash = http::http_header_name_hash(kTraceIdLowcaseHeader);
constexpr std::string_view kParentSpanIdLowcaseHeader = "hi-span-id-parent";
constexpr std::uint64_t kParentSpanIdHeaderHash = http::http_header_name_hash(kParentSpanIdLowcaseHeader);
constexpr std::string_view kSpanIdLowcaseHeader = "hi-span-id";
constexpr std::uint64_t kSpanIdHeaderHash = http::http_header_name_hash(kSpanIdLowcaseHeader);
constexpr std::size_t kMaxUserAgentBytes = 1024;

const AccessLogPolicy &default_access_log_policy() noexcept {
    static const AccessLogPolicy policy;
    return policy;
}

std::uint32_t next_access_log_sample() noexcept {
    static thread_local std::uint64_t sequence = []() noexcept {
        const event::EventLoop &loop = event::EventLoop::current();
        const std::uint64_t worker = loop.has_group_index() ? static_cast<std::uint64_t>(loop.group_index()) + 1 : 1;
        return worker * 0x9e3779b97f4a7c15ULL;
    }();
    std::uint64_t value = ++sequence;
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<std::uint32_t>(value % kAccessLogSampleScale);
}

bool has_inbound_context(const cat::MessageTraceContext &context) noexcept {
    return !context.message_id.empty() || !context.root_message_id.empty() || !context.parent_message_id.empty();
}

cat::MessageTraceContext read_trace_context(const http::HttpHeaders &headers) noexcept {
    return {
            .message_id = headers.get(kSpanIdLowcaseHeader, kSpanIdHeaderHash),
            .root_message_id = headers.get(kTraceIdLowcaseHeader, kTraceIdHeaderHash),
            .parent_message_id = headers.get(kParentSpanIdLowcaseHeader, kParentSpanIdHeaderHash),
    };
}

std::string_view response_result(const http::HttpResponseStats &response) noexcept {
    if (response.terminal_error != common::IoErr::None || !response.completed) {
        return "canceled";
    }
    if (response.status_code >= 200 && response.status_code < 400) {
        return "success";
    }
    if (response.status_code >= 400 && response.status_code < 500) {
        return "client_error";
    }
    return "server_error";
}

} // namespace

RequestObservability::RequestObservability(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                                           cat::CatClient *cat_client, const AccessLogPolicy *access_log_policy,
                                           const ClientMetadata &client_metadata, TracePropagation &trace) noexcept :
    metrics_(metrics), access_log_policy_(access_log_policy ? access_log_policy : &default_access_log_policy()),
    started_(event::EventLoop::current().now()) {
    if (metrics_) {
        metrics_->request_started();
    }
    trace.initialize(exchange);
    if (!cat_client) {
        return;
    }

    const cat::MessageTraceContext inbound = read_trace_context(exchange.request_headers());
    const bool inherited = has_inbound_context(inbound);
    bool invalid_fallback = false;
    auto created =
            cat_client->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path, {.context = inbound});
    if (!created && inherited &&
        (created.error() == cat::RecordError::InvalidContext || created.error() == cat::RecordError::LimitExceeded)) {
        invalid_fallback = true;
        created = cat_client->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path);
    }
    if (!created) {
        return;
    }
    root_ = std::move(*created);
    trace.attach_cat(root_);

    (void) root_.set_data_separator(' ');
    add_root_data("method", exchange.method_view());
    add_root_data("host", exchange.header("Host"));
    add_root_data("path", exchange.uri().path);
    add_root_data("content_type", exchange.header("Content-Type"));
    const std::string peer_ip = client_metadata.peer_address.to_string();
    const std::string client_ip =
            client_metadata.has_client_address ? client_metadata.client_address.to_string() : std::string{};
    add_root_data("realIp", client_ip);
    add_root_data("clientIp", client_ip);
    add_root_data("peerIp", peer_ip);
    add_root_data("clientScheme", client_metadata.external_scheme);
    add_root_data("clientAddressSource", client_address_source_name(client_metadata.address_source));
    add_root_data("clientSchemeSource", client_scheme_source_name(client_metadata.scheme_source));
    add_root_data("forwardingStatus", forwarding_status_name(client_metadata.forwarding_status));
    add_root_data("traceparent", trace.trace_parent());
    const std::string_view user_agent = exchange.header("User-Agent");
    if (!user_agent.empty()) {
        if (user_agent.size() > kMaxUserAgentBytes) {
            add_root_data("userAgentTruncated", "true");
        }
        add_root_data("userAgent", user_agent.substr(0, kMaxUserAgentBytes));
    }
    add_root_data("trace_context", invalid_fallback ? "invalid_fallback" : (inherited ? "continued" : "new"));
}

void RequestObservability::finish(http::HttpExchange &exchange, const ClientMetadata &client_metadata,
                                  std::string_view trace_id) noexcept {
    if (finished_) {
        return;
    }
    finished_ = true;

    const auto finished = event::EventLoop::current().now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(finished - started_);
    const http::HttpResponseStats &response = exchange.response_stats();
    if (metrics_) {
        metrics_->request_finished(response, duration);
    }

    std::array<char, std::numeric_limits<int>::digits10 + 3> status_buffer{};
    const auto status =
            std::to_chars(status_buffer.data(), status_buffer.data() + status_buffer.size(), response.status_code);
    const std::string_view status_text =
            status.ec == std::errc{} ? std::string_view(status_buffer.data(),
                                                        static_cast<std::size_t>(status.ptr - status_buffer.data()))
                                     : std::string_view("0");
    add_root_data("status", status_text);
    if (response.terminal_error != common::IoErr::None) {
        add_root_data("io_error", common::io_err_name(response.terminal_error));
    }
    if (root_.valid()) {
        const bool success = !execution_failed_ && response.completed && response.terminal_error == common::IoErr::None;
        (void) root_.complete(success ? cat::status::Success : cat::status::Error);
    }

    const bool failed = execution_failed_ || !response.completed || response.terminal_error != common::IoErr::None ||
                        response.status_code >= 400;
    const std::uint32_t sample = access_log_policy_->options().success_sample_rate_bps >= kAccessLogSampleScale
                                         ? 0
                                         : next_access_log_sample();
    if (access_log_policy_->should_log(failed, sample) && LOG_ACCESS.get().enabled(log::LogLevel::Info)) {
        const AccessLogUri uri = access_log_policy_->render_uri(exchange.uri());
        const std::string peer_ip = client_metadata.peer_address.to_string();
        const std::string client_ip =
                client_metadata.has_client_address ? client_metadata.client_address.to_string() : std::string{};
        LOG(LOG_ACCESS, INFO) << "request completed"
                              << " trace_id=" << log::quoted(trace_id)
                              << " method=" << log::quoted(exchange.method_view())
                              << " host=" << log::quoted(exchange.header("Host")) << " path=" << log::quoted(uri.path())
                              << " query=" << log::quoted(uri.query) << " query_hash=" << log::quoted(uri.query_hash)
                              << " query_filtered=" << uri.query_filtered << " query_redacted=" << uri.query_redacted
                              << " path_truncated=" << uri.path_truncated << " query_truncated=" << uri.query_truncated
                              << " query_hash_failed=" << uri.query_hash_failed << " project=" << log::quoted(project_)
                              << " client_ip=" << log::quoted(client_ip) << " peer_ip=" << log::quoted(peer_ip)
                              << " client_scheme=" << log::quoted(client_metadata.external_scheme)
                              << " client_address_source=" << client_address_source_name(client_metadata.address_source)
                              << " client_scheme_source=" << client_scheme_source_name(client_metadata.scheme_source)
                              << " forwarding_status=" << forwarding_status_name(client_metadata.forwarding_status)
                              << " route=" << log::quoted(route_) << " cluster=" << log::quoted(cluster_)
                              << " upstream=" << log::quoted(upstream_)
                              << " response_compression=" << log::quoted(response_compression_)
                              << " status=" << response.status_code << " result=" << response_result(response)
                              << " error=" << log::quoted(error_)
                              << " duration_us=" << std::max<std::int64_t>(duration.count(), 0)
                              << " response_bytes=" << response.body_bytes_sent
                              << " io_error=" << common::io_err_name(response.terminal_error);
    }
}

void RequestObservability::record_response_compression(bool compressed) noexcept {
    response_compression_ = compressed ? std::string_view("gzip") : std::string_view("identity");
    add_root_data("responseCompression", response_compression_);
    if (metrics_) {
        metrics_->response_compression_selected(compressed);
    }
}

void RequestObservability::record_response_compression_not_acceptable() noexcept {
    response_compression_ = "not_acceptable";
    add_root_data("responseCompression", response_compression_);
    if (metrics_) {
        metrics_->response_compression_not_acceptable();
    }
}

void RequestObservability::record_proxy_execution(AccessProxyExecutionResult result) noexcept {
    if (metrics_) {
        metrics_->proxy_execution_finished(result);
    }
}

void RequestObservability::record_proxy_attempt_started() noexcept {
    if (metrics_) {
        metrics_->proxy_attempt_started();
    }
}

void RequestObservability::record_proxy_attempt_finished(AccessProxyAttemptResult result) noexcept {
    if (metrics_) {
        metrics_->proxy_attempt_finished(result);
    }
}

void RequestObservability::record_proxy_failure(AccessProxyFailurePhase phase) noexcept {
    if (metrics_) {
        metrics_->proxy_failure(phase);
    }
}

void RequestObservability::record_proxy_connection(const ProxyConnectionObservation &observation) noexcept {
    if (!metrics_) {
        return;
    }
    if (observation.pool_hits != 0) {
        metrics_->proxy_pool_acquired(AccessProxyPoolResult::Hit, observation.pool_hits);
    }
    if (observation.pool_misses != 0) {
        metrics_->proxy_pool_acquired(AccessProxyPoolResult::Miss, observation.pool_misses);
    }
    if (observation.pool_shutdown != 0) {
        metrics_->proxy_pool_acquired(AccessProxyPoolResult::Shutdown, observation.pool_shutdown);
    }
    if (observation.dns_success != 0) {
        metrics_->proxy_dns_resolved(AccessProxyDnsResult::Success, observation.dns_success);
    }
    if (observation.dns_empty != 0) {
        metrics_->proxy_dns_resolved(AccessProxyDnsResult::Empty, observation.dns_empty);
    }
    if (observation.dns_failure != 0) {
        metrics_->proxy_dns_resolved(AccessProxyDnsResult::Failure, observation.dns_failure);
    }
    if (observation.dns_unavailable != 0) {
        metrics_->proxy_dns_resolved(AccessProxyDnsResult::Unavailable, observation.dns_unavailable);
    }
    if (observation.connect_success != 0) {
        metrics_->proxy_connect_attempted(AccessProxyConnectResult::Success, observation.connect_success);
    }
    if (observation.connect_failure != 0) {
        metrics_->proxy_connect_attempted(AccessProxyConnectResult::Failure, observation.connect_failure);
    }
    if (observation.tls_failure != 0) {
        metrics_->proxy_connect_attempted(AccessProxyConnectResult::TlsFailure, observation.tls_failure);
    }
    if (observation.create_failure != 0) {
        metrics_->proxy_connect_attempted(AccessProxyConnectResult::CreateFailure, observation.create_failure);
    }
}

void RequestObservability::record_websocket_handshake(AccessWebSocketHandshakeResult result) noexcept {
    if (metrics_) {
        metrics_->websocket_handshake_finished(result);
    }
}

void RequestObservability::record_websocket_session_started() noexcept {
    if (metrics_) {
        metrics_->websocket_session_started();
    }
}

void RequestObservability::record_websocket_session_finished(AccessWebSocketSessionResult result) noexcept {
    if (metrics_) {
        metrics_->websocket_session_finished(result);
    }
}

void RequestObservability::set_project(ScriptExecutionContext &execution, std::string_view project,
                                       std::string_view effective_host, std::string_view context_cluster) noexcept {
    project_ = execution.copy_to_request_pool(project);
    cluster_ = execution.copy_to_request_pool(context_cluster);
    add_root_data("project", project);
    add_root_data("effectiveHost", effective_host);
    if (!context_cluster.empty()) {
        add_root_data("cluster", context_cluster);
    }
    update_transaction_name(execution);
}

void RequestObservability::set_route(ScriptExecutionContext &execution, const CompiledRoute &route) noexcept {
    route_ = execution.copy_to_request_pool(route.path);
    add_root_data("route", route.path);
    update_transaction_name(execution);
}

void RequestObservability::mark_failed(ScriptExecutionContext &execution, std::string_view error) noexcept {
    execution_failed_ = true;
    if (failure_recorded_) {
        return;
    }
    failure_recorded_ = true;
    error_ = execution.copy_to_request_pool(error);
    add_root_data("error", error);
}

void RequestObservability::record_exception(ScriptExecutionContext &execution, const Exception &exception) noexcept {
    mark_failed(execution, exception.name);
    if (exception_recorded_) {
        return;
    }
    exception_recorded_ = true;
    if (root_.valid()) {
        auto event = root_.start_event("FiberException", exception.name);
        if (event) {
            (void) event->add_data(exception.message);
            (void) event->complete(cat::status::Error);
        }
    }
}

void RequestObservability::record_upstream_exception(ScriptExecutionContext &execution,
                                                     const Exception &exception) noexcept {
    mark_failed(execution, exception.name);
}

void RequestObservability::record_response_error(ScriptExecutionContext &execution, common::IoErr error) noexcept {
    mark_failed(execution, "RESPONSE_ERROR");
    if (response_error_recorded_) {
        return;
    }
    response_error_recorded_ = true;
    if (!root_.valid()) {
        return;
    }
    const std::string_view error_name = common::io_err_name(error);
    auto event = root_.start_event("ResponseError", error_name);
    if (event) {
        (void) event->add_data("io_error", error_name);
        (void) event->complete(cat::status::Error);
    }
}

void RequestObservability::mark_io_error(ScriptExecutionContext &execution, common::IoErr error) noexcept {
    mark_failed(execution, common::io_err_name(error));
}

void RequestObservability::set_upstream(ScriptExecutionContext &execution,
                                        const ProxyUpstreamEndpoint &endpoint) noexcept {
    upstream_ = execution.copy_to_request_pool(endpoint.host_header);
    add_root_data("upstream", endpoint.host_header);
}

AccessProviderTransaction RequestObservability::start_provider_transaction(std::string_view name) noexcept {
    if (!root_.valid()) {
        return {};
    }
    auto transaction = root_.start_transaction("Access.Provider", name);
    if (!transaction) {
        return {};
    }
    return AccessProviderTransaction(std::move(*transaction));
}

void RequestObservability::add_root_data(std::string_view key, std::string_view value) noexcept {
    if (root_.valid() && !value.empty()) {
        (void) root_.add_data(key, value);
    }
}

void RequestObservability::update_transaction_name(ScriptExecutionContext &execution) noexcept {
    if (!root_.valid() || project_.empty()) {
        return;
    }
    if (route_.empty()) {
        (void) root_.set_name(project_);
        return;
    }
    if (project_.size() > std::numeric_limits<std::size_t>::max() - route_.size()) {
        return;
    }
    const std::size_t size = project_.size() + route_.size();
    char *name = execution.pool().alloc<char>(size);
    if (!name) {
        return;
    }
    std::memcpy(name, project_.data(), project_.size());
    std::memcpy(name + project_.size(), route_.data(), route_.size());
    (void) root_.set_name(std::string_view(name, size));
}

} // namespace fiber::access_server
