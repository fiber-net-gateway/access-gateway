#include "AccessServerMetrics.h"
#include "AccessRuntimeMetrics.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

std::size_t request_result_index(const http::HttpResponseStats &response) noexcept {
    if (response.terminal_error != common::IoErr::None || !response.completed) {
        return 3;
    }
    if (response.status_code >= 200 && response.status_code < 400) {
        return 0;
    }
    if (response.status_code >= 400 && response.status_code < 500) {
        return 1;
    }
    return 2;
}

std::size_t request_status_index(int status_code) noexcept {
    switch (status_code) {
        case 200:
            return static_cast<std::size_t>(AccessResponseStatus::Code200);
        case 201:
            return static_cast<std::size_t>(AccessResponseStatus::Code201);
        case 202:
            return static_cast<std::size_t>(AccessResponseStatus::Code202);
        case 204:
            return static_cast<std::size_t>(AccessResponseStatus::Code204);
        case 206:
            return static_cast<std::size_t>(AccessResponseStatus::Code206);
        case 301:
            return static_cast<std::size_t>(AccessResponseStatus::Code301);
        case 302:
            return static_cast<std::size_t>(AccessResponseStatus::Code302);
        case 303:
            return static_cast<std::size_t>(AccessResponseStatus::Code303);
        case 304:
            return static_cast<std::size_t>(AccessResponseStatus::Code304);
        case 307:
            return static_cast<std::size_t>(AccessResponseStatus::Code307);
        case 308:
            return static_cast<std::size_t>(AccessResponseStatus::Code308);
        case 400:
            return static_cast<std::size_t>(AccessResponseStatus::Code400);
        case 401:
            return static_cast<std::size_t>(AccessResponseStatus::Code401);
        case 403:
            return static_cast<std::size_t>(AccessResponseStatus::Code403);
        case 404:
            return static_cast<std::size_t>(AccessResponseStatus::Code404);
        case 405:
            return static_cast<std::size_t>(AccessResponseStatus::Code405);
        case 406:
            return static_cast<std::size_t>(AccessResponseStatus::Code406);
        case 408:
            return static_cast<std::size_t>(AccessResponseStatus::Code408);
        case 409:
            return static_cast<std::size_t>(AccessResponseStatus::Code409);
        case 410:
            return static_cast<std::size_t>(AccessResponseStatus::Code410);
        case 412:
            return static_cast<std::size_t>(AccessResponseStatus::Code412);
        case 413:
            return static_cast<std::size_t>(AccessResponseStatus::Code413);
        case 415:
            return static_cast<std::size_t>(AccessResponseStatus::Code415);
        case 416:
            return static_cast<std::size_t>(AccessResponseStatus::Code416);
        case 422:
            return static_cast<std::size_t>(AccessResponseStatus::Code422);
        case 426:
            return static_cast<std::size_t>(AccessResponseStatus::Code426);
        case 429:
            return static_cast<std::size_t>(AccessResponseStatus::Code429);
        case 499:
            return static_cast<std::size_t>(AccessResponseStatus::Code499);
        case 500:
            return static_cast<std::size_t>(AccessResponseStatus::Code500);
        case 501:
            return static_cast<std::size_t>(AccessResponseStatus::Code501);
        case 502:
            return static_cast<std::size_t>(AccessResponseStatus::Code502);
        case 503:
            return static_cast<std::size_t>(AccessResponseStatus::Code503);
        case 504:
            return static_cast<std::size_t>(AccessResponseStatus::Code504);
        case 505:
            return static_cast<std::size_t>(AccessResponseStatus::Code505);
        default:
            break;
    }
    if (status_code >= 100 && status_code < 200) {
        return static_cast<std::size_t>(AccessResponseStatus::Other1xx);
    }
    if (status_code >= 200 && status_code < 300) {
        return static_cast<std::size_t>(AccessResponseStatus::Other2xx);
    }
    if (status_code >= 300 && status_code < 400) {
        return static_cast<std::size_t>(AccessResponseStatus::Other3xx);
    }
    if (status_code >= 400 && status_code < 500) {
        return static_cast<std::size_t>(AccessResponseStatus::Other4xx);
    }
    if (status_code >= 500 && status_code < 600) {
        return static_cast<std::size_t>(AccessResponseStatus::Other5xx);
    }
    return static_cast<std::size_t>(AccessResponseStatus::Other);
}

constexpr std::array<std::string_view, 40> kRequestStatusLabelValues{
        "200",   "201", "202", "204", "206", //
        "301",   "302", "303", "304", "307", "308", //
        "400",   "401", "403", "404", "405", "406", "408", "409", "410", "412", "413", "415", "416", //
        "422",   "426", "429", "499", //
        "500",   "501", "502", "503", "504", "505", //
        "1xx",   "2xx", "3xx", "4xx", "5xx", //
        "other", //
};
static_assert(kRequestStatusLabelValues.size() == static_cast<std::size_t>(AccessResponseStatus::Count));

constexpr std::string_view kRouteUnmatchedLabel = "unmatched";

constexpr std::array<std::string_view, 2> kBodyDirectionValues{
        "request",
        "response",
};
static_assert(kBodyDirectionValues.size() == static_cast<std::size_t>(AccessBodyDirection::Count));

// Hard caps for the dynamic LRU families. The route family is bounded by
// configured host patterns x route paths x observed statuses; provider
// families churn with discovery instances and get the largest allowance.
constexpr std::size_t kRouteRequestSeriesLimit = 8192;
constexpr std::size_t kProviderResponseSeriesLimit = 16384;
constexpr std::size_t kProxyErrorSeriesLimit = 2048;
constexpr std::size_t kCircuitRejectionSeriesLimit = 1024;
constexpr std::size_t kRouteBodySeriesLimit = 8192;
constexpr std::size_t kProviderBodySeriesLimit = 16384;

template<std::size_t N>
bool register_labeled_series(prometheus::MetricsRegistry &registry, prometheus::FamilyId family,
                             const std::array<std::string_view, N> &values,
                             std::array<prometheus::SeriesId, N> &series) {
    for (std::size_t i = 0; i < N; ++i) {
        auto registered = registry.register_series(family, std::array<std::string_view, 1>{values[i]});
        if (!registered) {
            return false;
        }
        series[i] = *registered;
    }
    return true;
}

template<std::size_t N>
bool bind_counters(prometheus::MetricsShard &shard, const std::array<prometheus::SeriesId, N> &series,
                   std::array<prometheus::CounterRef, N> &counters) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        auto value = shard.counter(series[i]);
        if (!value) {
            return false;
        }
        counters[i] = *value;
    }
    return true;
}

template<typename Enum>
std::size_t metric_index(Enum value, Enum count) noexcept {
    const std::size_t index = static_cast<std::size_t>(value);
    FIBER_ASSERT(index < static_cast<std::size_t>(count));
    return index;
}

} // namespace

void AccessServerMetrics::Worker::request_started() noexcept { inflight_.inc(); }

void AccessServerMetrics::Worker::request_finished(const http::HttpResponseStats &response,
                                                   std::chrono::microseconds duration) noexcept {
    inflight_.dec();
    requests_[request_result_index(response)].inc();
    if (response.terminal_error == common::IoErr::None && response.completed) {
        requests_by_status_[request_status_index(response.status_code)].inc();
    }
    request_duration_.observe(static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count(), 0)));
}

void AccessServerMetrics::Worker::response_compression_selected(bool compressed) noexcept {
    response_compression_[compressed ? 0 : 1].inc();
}

void AccessServerMetrics::Worker::response_compression_not_acceptable() noexcept { response_compression_[2].inc(); }

void AccessServerMetrics::Worker::proxy_execution_finished(AccessProxyExecutionResult result) noexcept {
    proxy_executions_[metric_index(result, AccessProxyExecutionResult::Count)].inc();
}

void AccessServerMetrics::Worker::proxy_attempt_started() noexcept { proxy_attempts_inflight_.inc(); }

void AccessServerMetrics::Worker::proxy_attempt_finished(AccessProxyAttemptResult result) noexcept {
    proxy_attempts_inflight_.dec();
    proxy_attempts_[metric_index(result, AccessProxyAttemptResult::Count)].inc();
}

void AccessServerMetrics::Worker::proxy_failure(AccessProxyFailurePhase phase) noexcept {
    proxy_failures_[metric_index(phase, AccessProxyFailurePhase::Count)].inc();
}

void AccessServerMetrics::Worker::proxy_pool_acquired(AccessProxyPoolResult result, std::uint64_t count) noexcept {
    proxy_pool_acquires_[metric_index(result, AccessProxyPoolResult::Count)].add(count);
}

void AccessServerMetrics::Worker::proxy_dns_resolved(AccessProxyDnsResult result, std::uint64_t count) noexcept {
    proxy_dns_resolutions_[metric_index(result, AccessProxyDnsResult::Count)].add(count);
}

void AccessServerMetrics::Worker::proxy_connect_attempted(AccessProxyConnectResult result,
                                                          std::uint64_t count) noexcept {
    proxy_connect_attempts_[metric_index(result, AccessProxyConnectResult::Count)].add(count);
}

void AccessServerMetrics::Worker::proxy_connect_candidates_observed(std::uint64_t count) noexcept {
    proxy_connect_candidates_.add(count);
}

void AccessServerMetrics::Worker::happy_eyeballs_finished(AccessHappyEyeballsResult result,
                                                          std::uint64_t count) noexcept {
    happy_eyeballs_[metric_index(result, AccessHappyEyeballsResult::Count)].add(count);
}

void AccessServerMetrics::Worker::websocket_handshake_finished(AccessWebSocketHandshakeResult result) noexcept {
    websocket_handshakes_[metric_index(result, AccessWebSocketHandshakeResult::Count)].inc();
}

void AccessServerMetrics::Worker::websocket_session_started() noexcept { websocket_sessions_inflight_.inc(); }

void AccessServerMetrics::Worker::websocket_session_finished(AccessWebSocketSessionResult result) noexcept {
    websocket_sessions_inflight_.dec();
    websocket_sessions_[metric_index(result, AccessWebSocketSessionResult::Count)].inc();
}

void AccessServerMetrics::Worker::proxy_body_idle_timeout(AccessBodyIdleDirection direction) noexcept {
    proxy_body_idle_timeouts_[metric_index(direction, AccessBodyIdleDirection::Count)].inc();
}

void AccessServerMetrics::Worker::request_body_transferred(AccessBodySide side, std::uint64_t bytes) noexcept {
    if (bytes != 0) {
        request_body_bytes_[metric_index(side, AccessBodySide::Count)].add(bytes);
    }
}

void AccessServerMetrics::Worker::response_body_transferred(AccessBodySide side, std::uint64_t bytes) noexcept {
    if (bytes != 0) {
        response_body_bytes_[metric_index(side, AccessBodySide::Count)].add(bytes);
    }
}

void AccessServerMetrics::Worker::request_body_size(std::uint64_t bytes) noexcept {
    if (bytes != 0) {
        request_body_size_.observe(bytes);
    }
}

void AccessServerMetrics::Worker::response_body_size(std::uint64_t bytes) noexcept {
    if (bytes != 0) {
        response_body_size_.observe(bytes);
    }
}

void AccessServerMetrics::Worker::response_compression_bytes(AccessCompressionStream stream,
                                                             std::uint64_t bytes) noexcept {
    if (bytes != 0) {
        response_compression_bytes_[metric_index(stream, AccessCompressionStream::Count)].add(bytes);
    }
}

void AccessServerMetrics::Worker::route_request_finished(std::string_view host_pattern, std::string_view route_path,
                                                         std::string_view status_label) noexcept {
    if (!route_requests_dynamic_ || status_label.empty()) {
        return;
    }
    // The matched configuration pattern, not the client-controlled Host value,
    // keeps this family bounded by the published route model. Requests that
    // never matched a host or route keep a visible bucket instead of silently
    // diverging from access_server_requests_total.
    const std::array<std::string_view, 3> labels{
            host_pattern.empty() ? kRouteUnmatchedLabel : host_pattern,
            route_path.empty() ? kRouteUnmatchedLabel : route_path,
            status_label,
    };
    (void) route_requests_dynamic_->inc(labels);
}

void AccessServerMetrics::Worker::provider_response_received(std::string_view app, std::string_view ip_port,
                                                             std::string_view status_label) noexcept {
    if (!provider_responses_dynamic_) {
        return;
    }
    const std::array<std::string_view, 3> labels{app, ip_port, status_label};
    (void) provider_responses_dynamic_->inc(labels);
}

void AccessServerMetrics::Worker::proxy_transport_error(std::string_view app, std::string_view ip_port,
                                                        std::string_view error_label) noexcept {
    if (!proxy_errors_dynamic_ || error_label.empty()) {
        return;
    }
    const std::array<std::string_view, 3> labels{app, ip_port, error_label};
    (void) proxy_errors_dynamic_->inc(labels);
}

void AccessServerMetrics::Worker::upstream_circuit_rejected(std::string_view app) noexcept {
    if (!circuit_rejections_dynamic_ || app.empty()) {
        return;
    }
    const std::array<std::string_view, 1> labels{app};
    (void) circuit_rejections_dynamic_->inc(labels);
}

void AccessServerMetrics::Worker::route_body_transferred(std::string_view host_pattern, std::string_view route_path,
                                                         AccessBodyDirection direction, std::uint64_t bytes) noexcept {
    if (!route_body_bytes_dynamic_ || bytes == 0) {
        return;
    }
    const std::array<std::string_view, 3> labels{
            host_pattern.empty() ? kRouteUnmatchedLabel : host_pattern,
            route_path.empty() ? kRouteUnmatchedLabel : route_path,
            kBodyDirectionValues[metric_index(direction, AccessBodyDirection::Count)],
    };
    (void) route_body_bytes_dynamic_->add(labels, bytes);
}

void AccessServerMetrics::Worker::provider_body_transferred(std::string_view app, std::string_view ip_port,
                                                            AccessBodyDirection direction,
                                                            std::uint64_t bytes) noexcept {
    if (!provider_body_bytes_dynamic_ || bytes == 0 || app.empty()) {
        return;
    }
    const std::array<std::string_view, 3> labels{
            app, ip_port, kBodyDirectionValues[metric_index(direction, AccessBodyDirection::Count)]};
    (void) provider_body_bytes_dynamic_->add(labels, bytes);
}

AccessServerMetrics::AccessServerMetrics(event::EventLoopGroup &workers, const AccessRuntimeMetrics *runtime_metrics) :
    runtime_metrics_(runtime_metrics) {
    valid_ = initialize(workers);
}

AccessServerMetrics::~AccessServerMetrics() { FIBER_ASSERT(!valid_ || collecting_stopped_); }

bool AccessServerMetrics::initialize(event::EventLoopGroup &worker_group) {
    constexpr std::array<std::string_view, 1> kResultLabel{"result"};
    constexpr std::array<std::string_view, 1> kPhaseLabel{"phase"};
    constexpr std::array<std::string_view, 1> kStatusCodeLabel{"status_code"};
    constexpr std::array<std::string_view, 4> kRequestResults{
            "success",
            "client_error",
            "server_error",
            "canceled",
    };
    constexpr std::array<std::string_view, 3> kCompressionResults{
            "gzip",
            "identity",
            "not_acceptable",
    };
    constexpr std::array<std::string_view, 3> kProxyExecutionResults{
            "completed",
            "failed",
            "canceled",
    };
    constexpr std::array<std::string_view, 3> kProxyAttemptResults{
            "completed",
            "failed",
            "aborted",
    };
    constexpr std::array<std::string_view, 21> kProxyFailurePhases{
            "no_upstream_hosts",
            "upstream_circuit_open",
            "invalid_selection",
            "evaluate_context",
            "resolve_upstream",
            "pool_shutdown",
            "connect",
            "tls",
            "build_request",
            "build_headers",
            "send_header",
            "read_request_body",
            "request_body_too_large",
            "send_request_body",
            "read_response_header",
            "build_response_headers",
            "response_body_too_large",
            "switch_websocket",
            "send_response_header",
            "read_response_body",
            "write_response_body",
    };
    constexpr std::array<std::string_view, 3> kProxyPoolResults{
            "hit",
            "miss",
            "shutdown",
    };
    constexpr std::array<std::string_view, 4> kProxyDnsResults{
            "success",
            "empty",
            "failure",
            "unavailable",
    };
    constexpr std::array<std::string_view, 4> kProxyConnectResults{
            "success",
            "failure",
            "tls_failure",
            "create_failure",
    };
    constexpr std::array<std::string_view, 2> kHappyEyeballsResults{
            "success",
            "failure",
    };
    constexpr std::array<std::string_view, 3> kWebSocketHandshakeResults{
            "accepted",
            "rejected",
            "failed",
    };
    constexpr std::array<std::string_view, 2> kWebSocketSessionResults{
            "closed",
            "aborted",
    };
    constexpr std::array<std::string_view, 2> kBodyIdleDirections{
            "request_body",
            "response_body",
    };
    constexpr std::array<std::string_view, 1> kDirectionLabel{"direction"};
    static_assert(kBodyIdleDirections.size() == static_cast<std::size_t>(AccessBodyIdleDirection::Count));
    constexpr std::array<std::string_view, 2> kBodySides{
            "downstream",
            "upstream",
    };
    static_assert(kBodySides.size() == static_cast<std::size_t>(AccessBodySide::Count));
    constexpr std::array<std::string_view, 2> kCompressionStreams{
            "raw",
            "compressed",
    };
    static_assert(kCompressionStreams.size() == static_cast<std::size_t>(AccessCompressionStream::Count));
    constexpr std::array<std::string_view, 1> kSideLabel{"side"};
    constexpr std::array<std::string_view, 1> kStreamLabel{"stream"};
    // Byte-scale buckets spanning API-sized payloads (KiB) through dataset
    // uploads and the configured response-body ceilings (hundreds of MiB).
    constexpr std::array<std::uint64_t, 11> kBodySizeBounds{
            1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216, 67108864, 268435456, 1073741824,
    };
    constexpr std::array<std::uint64_t, 15> kDurationBounds{
            1000,    5000,    10000,   25000,    50000,    100000,   250000,    500000,
            1000000, 2500000, 5000000, 10000000, 30000000, 60000000, 300000000,
    };
    static_assert(kProxyExecutionResults.size() == static_cast<std::size_t>(AccessProxyExecutionResult::Count));
    static_assert(kProxyAttemptResults.size() == static_cast<std::size_t>(AccessProxyAttemptResult::Count));
    static_assert(kProxyFailurePhases.size() == static_cast<std::size_t>(AccessProxyFailurePhase::Count));
    static_assert(kProxyPoolResults.size() == static_cast<std::size_t>(AccessProxyPoolResult::Count));
    static_assert(kProxyDnsResults.size() == static_cast<std::size_t>(AccessProxyDnsResult::Count));
    static_assert(kProxyConnectResults.size() == static_cast<std::size_t>(AccessProxyConnectResult::Count));
    static_assert(kHappyEyeballsResults.size() == static_cast<std::size_t>(AccessHappyEyeballsResult::Count));
    static_assert(kWebSocketHandshakeResults.size() == static_cast<std::size_t>(AccessWebSocketHandshakeResult::Count));
    static_assert(kWebSocketSessionResults.size() == static_cast<std::size_t>(AccessWebSocketSessionResult::Count));

    auto requests = registry_.register_counter("access_server_requests_total", "Completed access-server requests.",
                                               kResultLabel);
    auto requests_by_status =
            registry_.register_counter("access_server_requests_by_status_total",
                                       "Completed access-server requests by response status code.", kStatusCodeLabel);
    auto response_compression =
            registry_.register_counter("access_server_response_compression_total",
                                       "Negotiated outcomes for gzip-enabled RESPONSE routes.", kResultLabel);
    auto duration =
            registry_.register_histogram("access_server_request_duration_seconds", "Access-server request duration.",
                                         kDurationBounds, prometheus::HistogramUnit::Microseconds);
    auto inflight = registry_.register_gauge("access_server_requests_inflight", "In-flight access-server requests.",
                                             prometheus::GaugeReduction::Sum);
    auto proxy_executions = registry_.register_counter("access_server_proxy_executions_total",
                                                       "Terminal outcomes for proxy route executions.", kResultLabel);
    auto proxy_attempts =
            registry_.register_counter("access_server_proxy_attempts_total",
                                       "Terminal outcomes for selected-upstream proxy attempts.", kResultLabel);
    auto proxy_attempts_inflight = registry_.register_gauge("access_server_proxy_attempts_inflight",
                                                            "Selected-upstream proxy attempts currently in flight.",
                                                            prometheus::GaugeReduction::Sum);
    auto proxy_failures =
            registry_.register_counter("access_server_proxy_failures_total",
                                       "Proxy failures classified by the phase that observed them.", kPhaseLabel);
    auto proxy_pool_acquires =
            registry_.register_counter("access_server_proxy_pool_acquires_total",
                                       "HTTP/1 upstream connection pool acquisition outcomes.", kResultLabel);
    auto proxy_dns_resolutions =
            registry_.register_counter("access_server_proxy_dns_resolutions_total",
                                       "Hostname resolution outcomes for proxy upstreams.", kResultLabel);
    auto proxy_connect_attempts =
            registry_.register_counter("access_server_proxy_connect_attempts_total",
                                       "New upstream transport connection attempt outcomes.", kResultLabel);
    auto proxy_connect_candidates = registry_.register_counter(
            "access_server_proxy_connect_candidates_total", "Address candidates supplied to new upstream connections.");
    auto happy_eyeballs = registry_.register_counter("access_server_proxy_happy_eyeballs_total",
                                                     "Multi-address upstream connection race outcomes.", kResultLabel);
    auto websocket_handshakes = registry_.register_counter("access_server_websocket_handshakes_total",
                                                           "WebSocket proxy handshake outcomes.", kResultLabel);
    auto websocket_sessions = registry_.register_counter("access_server_websocket_sessions_total",
                                                         "Completed WebSocket proxy session outcomes.", kResultLabel);
    auto proxy_body_idle_timeouts =
            registry_.register_counter("access_server_proxy_body_idle_timeouts_total",
                                       "Proxy body streams cut by the per-read inactivity timeout.", kDirectionLabel);
    auto websocket_sessions_inflight = registry_.register_gauge(
            "access_server_websocket_sessions_inflight", "Established WebSocket proxy sessions currently in flight.",
            prometheus::GaugeReduction::Sum);
    auto request_body_bytes = registry_.register_counter("access_server_request_body_bytes_total",
                                                         "Request body bytes by transfer leg: downstream accepted from "
                                                         "the client, upstream written to the provider.",
                                                         kSideLabel);
    auto response_body_bytes = registry_.register_counter(
            "access_server_response_body_bytes_total",
            "Response body bytes by transfer leg: upstream read from the provider, downstream written to the client.",
            kSideLabel);
    auto response_compression_bytes = registry_.register_counter(
            "access_server_response_compression_bytes_total",
            "Bytes fed through the gzip response path, raw versus the compressed wire output.", kStreamLabel);
    auto request_body_size = registry_.register_histogram("access_server_request_body_size_bytes",
                                                          "Request body sizes for transfers that moved bytes.",
                                                          kBodySizeBounds, prometheus::HistogramUnit::Raw);
    auto response_body_size = registry_.register_histogram("access_server_response_body_size_bytes",
                                                           "Response body sizes for transfers that moved bytes.",
                                                           kBodySizeBounds, prometheus::HistogramUnit::Raw);
    // Dynamic LRU families: label values arrive at request time and are capped
    // per family; eviction resets the evicted series (see bounded-metrics.md).
    constexpr std::array<std::string_view, 3> kRouteRequestLabels{"host", "path", "status_code"};
    constexpr std::array<std::string_view, 3> kProviderResponseLabels{"app", "ip_port", "status_code"};
    constexpr std::array<std::string_view, 3> kProxyErrorLabels{"app", "ip_port", "error"};
    constexpr std::array<std::string_view, 1> kCircuitRejectionLabels{"app"};
    constexpr std::array<std::string_view, 3> kRouteBodyLabels{"host", "path", "direction"};
    constexpr std::array<std::string_view, 3> kProviderBodyLabels{"app", "ip_port", "direction"};
    const prometheus::SeriesOptions route_request_options{
            .mode = prometheus::SeriesMode::Lru,
            .max_series = kRouteRequestSeriesLimit,
    };
    const prometheus::SeriesOptions provider_response_options{
            .mode = prometheus::SeriesMode::Lru,
            .max_series = kProviderResponseSeriesLimit,
    };
    const prometheus::SeriesOptions proxy_error_options{
            .mode = prometheus::SeriesMode::Lru,
            .max_series = kProxyErrorSeriesLimit,
    };
    const prometheus::SeriesOptions circuit_rejection_options{
            .mode = prometheus::SeriesMode::Lru,
            .max_series = kCircuitRejectionSeriesLimit,
    };
    const prometheus::SeriesOptions route_body_options{
            .mode = prometheus::SeriesMode::Lru,
            .max_series = kRouteBodySeriesLimit,
    };
    const prometheus::SeriesOptions provider_body_options{
            .mode = prometheus::SeriesMode::Lru,
            .max_series = kProviderBodySeriesLimit,
    };
    auto route_requests = registry_.register_counter(
            "access_server_route_requests_total",
            "Completed requests by matched host pattern, route path pattern, and final status code.",
            kRouteRequestLabels, route_request_options);
    auto provider_responses = registry_.register_counter(
            "access_server_provider_responses_total",
            "Upstream response heads received by provider app, upstream address, and status.", kProviderResponseLabels,
            provider_response_options);
    auto proxy_errors = registry_.register_counter(
            "access_server_proxy_errors_total",
            "Upstream transport failures by provider app, upstream address, and coarse error class.", kProxyErrorLabels,
            proxy_error_options);
    auto circuit_rejections =
            registry_.register_counter("access_server_upstream_circuit_rejections_total",
                                       "Upstream selections rejected because the endpoint circuit breaker was open.",
                                       kCircuitRejectionLabels, circuit_rejection_options);
    auto route_body_bytes = registry_.register_counter(
            "access_server_route_body_bytes_total",
            "Body bytes by matched host pattern and route path pattern: request bodies received from the client and "
            "response body wire bytes written to the client.",
            kRouteBodyLabels, route_body_options);
    auto provider_body_bytes = registry_.register_counter("access_server_provider_body_bytes_total",
                                                          "Per-attempt body bytes by provider app and upstream "
                                                          "address: request bodies written to and response bodies "
                                                          "read from that endpoint.",
                                                          kProviderBodyLabels, provider_body_options);
    if (!requests || !requests_by_status || !response_compression || !duration || !inflight || !proxy_executions ||
        !proxy_attempts || !proxy_attempts_inflight || !proxy_failures || !proxy_pool_acquires ||
        !proxy_dns_resolutions || !proxy_connect_attempts || !proxy_connect_candidates || !happy_eyeballs ||
        !websocket_handshakes || !websocket_sessions || !websocket_sessions_inflight || !proxy_body_idle_timeouts ||
        !route_requests || !provider_responses || !proxy_errors || !circuit_rejections || !request_body_bytes ||
        !response_body_bytes || !response_compression_bytes || !request_body_size || !response_body_size ||
        !route_body_bytes || !provider_body_bytes) {
        return false;
    }

    std::array<prometheus::SeriesId, kRequestResults.size()> request_series;
    std::array<prometheus::SeriesId, kRequestStatusLabelValues.size()> request_status_series;
    std::array<prometheus::SeriesId, kCompressionResults.size()> compression_series;
    std::array<prometheus::SeriesId, kProxyExecutionResults.size()> proxy_execution_series;
    std::array<prometheus::SeriesId, kProxyAttemptResults.size()> proxy_attempt_series;
    std::array<prometheus::SeriesId, kProxyFailurePhases.size()> proxy_failure_series;
    std::array<prometheus::SeriesId, kProxyPoolResults.size()> proxy_pool_series;
    std::array<prometheus::SeriesId, kProxyDnsResults.size()> proxy_dns_series;
    std::array<prometheus::SeriesId, kProxyConnectResults.size()> proxy_connect_series;
    std::array<prometheus::SeriesId, kHappyEyeballsResults.size()> happy_eyeballs_series;
    std::array<prometheus::SeriesId, kWebSocketHandshakeResults.size()> websocket_handshake_series;
    std::array<prometheus::SeriesId, kWebSocketSessionResults.size()> websocket_session_series;
    std::array<prometheus::SeriesId, kBodyIdleDirections.size()> proxy_body_idle_series;
    std::array<prometheus::SeriesId, kBodySides.size()> request_body_series;
    std::array<prometheus::SeriesId, kBodySides.size()> response_body_series;
    std::array<prometheus::SeriesId, kCompressionStreams.size()> response_compression_series;
    if (!register_labeled_series(registry_, *requests, kRequestResults, request_series) ||
        !register_labeled_series(registry_, *requests_by_status, kRequestStatusLabelValues, request_status_series) ||
        !register_labeled_series(registry_, *response_compression, kCompressionResults, compression_series) ||
        !register_labeled_series(registry_, *proxy_executions, kProxyExecutionResults, proxy_execution_series) ||
        !register_labeled_series(registry_, *proxy_attempts, kProxyAttemptResults, proxy_attempt_series) ||
        !register_labeled_series(registry_, *proxy_failures, kProxyFailurePhases, proxy_failure_series) ||
        !register_labeled_series(registry_, *proxy_pool_acquires, kProxyPoolResults, proxy_pool_series) ||
        !register_labeled_series(registry_, *proxy_dns_resolutions, kProxyDnsResults, proxy_dns_series) ||
        !register_labeled_series(registry_, *proxy_connect_attempts, kProxyConnectResults, proxy_connect_series) ||
        !register_labeled_series(registry_, *happy_eyeballs, kHappyEyeballsResults, happy_eyeballs_series) ||
        !register_labeled_series(registry_, *websocket_handshakes, kWebSocketHandshakeResults,
                                 websocket_handshake_series) ||
        !register_labeled_series(registry_, *websocket_sessions, kWebSocketSessionResults, websocket_session_series) ||
        !register_labeled_series(registry_, *proxy_body_idle_timeouts, kBodyIdleDirections, proxy_body_idle_series) ||
        !register_labeled_series(registry_, *request_body_bytes, kBodySides, request_body_series) ||
        !register_labeled_series(registry_, *response_body_bytes, kBodySides, response_body_series) ||
        !register_labeled_series(registry_, *response_compression_bytes, kCompressionStreams,
                                 response_compression_series)) {
        return false;
    }
    auto duration_series = registry_.register_series(*duration);
    auto inflight_series = registry_.register_series(*inflight);
    auto proxy_attempts_inflight_series = registry_.register_series(*proxy_attempts_inflight);
    auto proxy_connect_candidates_series = registry_.register_series(*proxy_connect_candidates);
    auto websocket_sessions_inflight_series = registry_.register_series(*websocket_sessions_inflight);
    auto request_body_size_series = registry_.register_series(*request_body_size);
    auto response_body_size_series = registry_.register_series(*response_body_size);
    if (!duration_series || !inflight_series || !proxy_attempts_inflight_series || !proxy_connect_candidates_series ||
        !websocket_sessions_inflight_series || !request_body_size_series || !response_body_size_series) {
        return false;
    }

    std::vector<prometheus::ShardId> shard_ids;
    shard_ids.reserve(worker_group.size());
    for (std::size_t i = 0; i < worker_group.size(); ++i) {
        auto shard = registry_.add_shard(worker_group.at(i));
        if (!shard) {
            return false;
        }
        shard_ids.push_back(*shard);
    }
    if (!registry_.freeze()) {
        return false;
    }

    auto route_requests_dynamic = registry_.dynamic_counter(*route_requests);
    auto provider_responses_dynamic = registry_.dynamic_counter(*provider_responses);
    auto proxy_errors_dynamic = registry_.dynamic_counter(*proxy_errors);
    auto circuit_rejections_dynamic = registry_.dynamic_counter(*circuit_rejections);
    auto route_body_bytes_dynamic = registry_.dynamic_counter(*route_body_bytes);
    auto provider_body_bytes_dynamic = registry_.dynamic_counter(*provider_body_bytes);
    if (!route_requests_dynamic || !provider_responses_dynamic || !proxy_errors_dynamic ||
        !circuit_rejections_dynamic || !route_body_bytes_dynamic || !provider_body_bytes_dynamic) {
        return false;
    }

    workers_.resize(worker_group.size());
    for (std::size_t worker_index = 0; worker_index < workers_.size(); ++worker_index) {
        prometheus::MetricsShard *shard = registry_.shard(shard_ids[worker_index]);
        if (!shard) {
            return false;
        }
        Worker &worker = workers_[worker_index];
        if (!bind_counters(*shard, request_series, worker.requests_) ||
            !bind_counters(*shard, request_status_series, worker.requests_by_status_) ||
            !bind_counters(*shard, compression_series, worker.response_compression_) ||
            !bind_counters(*shard, proxy_execution_series, worker.proxy_executions_) ||
            !bind_counters(*shard, proxy_attempt_series, worker.proxy_attempts_) ||
            !bind_counters(*shard, proxy_failure_series, worker.proxy_failures_) ||
            !bind_counters(*shard, proxy_pool_series, worker.proxy_pool_acquires_) ||
            !bind_counters(*shard, proxy_dns_series, worker.proxy_dns_resolutions_) ||
            !bind_counters(*shard, proxy_connect_series, worker.proxy_connect_attempts_) ||
            !bind_counters(*shard, happy_eyeballs_series, worker.happy_eyeballs_) ||
            !bind_counters(*shard, websocket_handshake_series, worker.websocket_handshakes_) ||
            !bind_counters(*shard, websocket_session_series, worker.websocket_sessions_) ||
            !bind_counters(*shard, proxy_body_idle_series, worker.proxy_body_idle_timeouts_) ||
            !bind_counters(*shard, request_body_series, worker.request_body_bytes_) ||
            !bind_counters(*shard, response_body_series, worker.response_body_bytes_) ||
            !bind_counters(*shard, response_compression_series, worker.response_compression_bytes_)) {
            return false;
        }
        auto duration_value = shard->histogram(*duration_series);
        auto inflight_value = shard->gauge(*inflight_series);
        auto proxy_attempts_inflight_value = shard->gauge(*proxy_attempts_inflight_series);
        auto proxy_connect_candidates_value = shard->counter(*proxy_connect_candidates_series);
        auto websocket_sessions_inflight_value = shard->gauge(*websocket_sessions_inflight_series);
        auto request_body_size_value = shard->histogram(*request_body_size_series);
        auto response_body_size_value = shard->histogram(*response_body_size_series);
        if (!duration_value || !inflight_value || !proxy_attempts_inflight_value || !proxy_connect_candidates_value ||
            !websocket_sessions_inflight_value || !request_body_size_value || !response_body_size_value) {
            return false;
        }
        worker.request_duration_ = *duration_value;
        worker.inflight_ = *inflight_value;
        worker.proxy_attempts_inflight_ = *proxy_attempts_inflight_value;
        worker.proxy_connect_candidates_ = *proxy_connect_candidates_value;
        worker.websocket_sessions_inflight_ = *websocket_sessions_inflight_value;
        worker.request_body_size_ = *request_body_size_value;
        worker.response_body_size_ = *response_body_size_value;
        worker.route_requests_dynamic_ = *route_requests_dynamic;
        worker.provider_responses_dynamic_ = *provider_responses_dynamic;
        worker.proxy_errors_dynamic_ = *proxy_errors_dynamic;
        worker.circuit_rejections_dynamic_ = *circuit_rejections_dynamic;
        worker.route_body_bytes_dynamic_ = *route_body_bytes_dynamic;
        worker.provider_body_bytes_dynamic_ = *provider_body_bytes_dynamic;
    }
    return true;
}

AccessServerMetrics::Worker &AccessServerMetrics::worker(std::size_t index) noexcept {
    FIBER_ASSERT(valid_);
    FIBER_ASSERT(index < workers_.size());
    return workers_[index];
}

async::Task<common::IoResult<mem::IoBufChain>> AccessServerMetrics::collect() noexcept {
    auto collected = co_await registry_.collect_text();
    if (!collected || !runtime_metrics_) {
        co_return collected;
    }

    std::string runtime_text;
    runtime_metrics_->append_prometheus(runtime_text, event::EventLoop::current().now());
    if (runtime_text.empty()) {
        co_return collected;
    }
    mem::IoBuf runtime_buffer = mem::IoBuf::allocate(runtime_text.size());
    if (!runtime_buffer) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    std::memcpy(runtime_buffer.writable_data(), runtime_text.data(), runtime_text.size());
    runtime_buffer.commit(runtime_text.size());
    if (!collected->append(std::move(runtime_buffer))) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    co_return collected;
}

void AccessServerMetrics::stop_collecting() noexcept {
    if (!valid_ || collecting_stopped_) {
        return;
    }
    registry_.stop_collecting();
    collecting_stopped_ = true;
}

async::Task<void> AccessServerMetrics::wait_for_idle() noexcept {
    if (valid_) {
        co_await registry_.wait_for_idle();
    }
}

} // namespace fiber::access_server
