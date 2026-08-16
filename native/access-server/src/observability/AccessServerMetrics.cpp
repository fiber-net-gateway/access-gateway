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

void AccessServerMetrics::Worker::websocket_handshake_finished(AccessWebSocketHandshakeResult result) noexcept {
    websocket_handshakes_[metric_index(result, AccessWebSocketHandshakeResult::Count)].inc();
}

void AccessServerMetrics::Worker::websocket_session_started() noexcept { websocket_sessions_inflight_.inc(); }

void AccessServerMetrics::Worker::websocket_session_finished(AccessWebSocketSessionResult result) noexcept {
    websocket_sessions_inflight_.dec();
    websocket_sessions_[metric_index(result, AccessWebSocketSessionResult::Count)].inc();
}

AccessServerMetrics::AccessServerMetrics(event::EventLoopGroup &workers, const AccessRuntimeMetrics *runtime_metrics) :
    runtime_metrics_(runtime_metrics) {
    valid_ = initialize(workers);
}

AccessServerMetrics::~AccessServerMetrics() { FIBER_ASSERT(!valid_ || collecting_stopped_); }

bool AccessServerMetrics::initialize(event::EventLoopGroup &worker_group) {
    constexpr std::array<std::string_view, 1> kResultLabel{"result"};
    constexpr std::array<std::string_view, 1> kPhaseLabel{"phase"};
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
    constexpr std::array<std::string_view, 3> kWebSocketHandshakeResults{
            "accepted",
            "rejected",
            "failed",
    };
    constexpr std::array<std::string_view, 2> kWebSocketSessionResults{
            "closed",
            "aborted",
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
    static_assert(kWebSocketHandshakeResults.size() == static_cast<std::size_t>(AccessWebSocketHandshakeResult::Count));
    static_assert(kWebSocketSessionResults.size() == static_cast<std::size_t>(AccessWebSocketSessionResult::Count));

    auto requests = registry_.register_counter("access_server_requests_total", "Completed access-server requests.",
                                               kResultLabel);
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
    auto websocket_handshakes = registry_.register_counter("access_server_websocket_handshakes_total",
                                                           "WebSocket proxy handshake outcomes.", kResultLabel);
    auto websocket_sessions = registry_.register_counter("access_server_websocket_sessions_total",
                                                         "Completed WebSocket proxy session outcomes.", kResultLabel);
    auto websocket_sessions_inflight = registry_.register_gauge(
            "access_server_websocket_sessions_inflight", "Established WebSocket proxy sessions currently in flight.",
            prometheus::GaugeReduction::Sum);
    if (!requests || !response_compression || !duration || !inflight || !proxy_executions || !proxy_attempts ||
        !proxy_attempts_inflight || !proxy_failures || !proxy_pool_acquires || !proxy_dns_resolutions ||
        !proxy_connect_attempts || !websocket_handshakes || !websocket_sessions || !websocket_sessions_inflight) {
        return false;
    }

    std::array<prometheus::SeriesId, kRequestResults.size()> request_series;
    std::array<prometheus::SeriesId, kCompressionResults.size()> compression_series;
    std::array<prometheus::SeriesId, kProxyExecutionResults.size()> proxy_execution_series;
    std::array<prometheus::SeriesId, kProxyAttemptResults.size()> proxy_attempt_series;
    std::array<prometheus::SeriesId, kProxyFailurePhases.size()> proxy_failure_series;
    std::array<prometheus::SeriesId, kProxyPoolResults.size()> proxy_pool_series;
    std::array<prometheus::SeriesId, kProxyDnsResults.size()> proxy_dns_series;
    std::array<prometheus::SeriesId, kProxyConnectResults.size()> proxy_connect_series;
    std::array<prometheus::SeriesId, kWebSocketHandshakeResults.size()> websocket_handshake_series;
    std::array<prometheus::SeriesId, kWebSocketSessionResults.size()> websocket_session_series;
    if (!register_labeled_series(registry_, *requests, kRequestResults, request_series) ||
        !register_labeled_series(registry_, *response_compression, kCompressionResults, compression_series) ||
        !register_labeled_series(registry_, *proxy_executions, kProxyExecutionResults, proxy_execution_series) ||
        !register_labeled_series(registry_, *proxy_attempts, kProxyAttemptResults, proxy_attempt_series) ||
        !register_labeled_series(registry_, *proxy_failures, kProxyFailurePhases, proxy_failure_series) ||
        !register_labeled_series(registry_, *proxy_pool_acquires, kProxyPoolResults, proxy_pool_series) ||
        !register_labeled_series(registry_, *proxy_dns_resolutions, kProxyDnsResults, proxy_dns_series) ||
        !register_labeled_series(registry_, *proxy_connect_attempts, kProxyConnectResults, proxy_connect_series) ||
        !register_labeled_series(registry_, *websocket_handshakes, kWebSocketHandshakeResults,
                                 websocket_handshake_series) ||
        !register_labeled_series(registry_, *websocket_sessions, kWebSocketSessionResults, websocket_session_series)) {
        return false;
    }
    auto duration_series = registry_.register_series(*duration);
    auto inflight_series = registry_.register_series(*inflight);
    auto proxy_attempts_inflight_series = registry_.register_series(*proxy_attempts_inflight);
    auto websocket_sessions_inflight_series = registry_.register_series(*websocket_sessions_inflight);
    if (!duration_series || !inflight_series || !proxy_attempts_inflight_series ||
        !websocket_sessions_inflight_series) {
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

    workers_.resize(worker_group.size());
    for (std::size_t worker_index = 0; worker_index < workers_.size(); ++worker_index) {
        prometheus::MetricsShard *shard = registry_.shard(shard_ids[worker_index]);
        if (!shard) {
            return false;
        }
        Worker &worker = workers_[worker_index];
        if (!bind_counters(*shard, request_series, worker.requests_) ||
            !bind_counters(*shard, compression_series, worker.response_compression_) ||
            !bind_counters(*shard, proxy_execution_series, worker.proxy_executions_) ||
            !bind_counters(*shard, proxy_attempt_series, worker.proxy_attempts_) ||
            !bind_counters(*shard, proxy_failure_series, worker.proxy_failures_) ||
            !bind_counters(*shard, proxy_pool_series, worker.proxy_pool_acquires_) ||
            !bind_counters(*shard, proxy_dns_series, worker.proxy_dns_resolutions_) ||
            !bind_counters(*shard, proxy_connect_series, worker.proxy_connect_attempts_) ||
            !bind_counters(*shard, websocket_handshake_series, worker.websocket_handshakes_) ||
            !bind_counters(*shard, websocket_session_series, worker.websocket_sessions_)) {
            return false;
        }
        auto duration_value = shard->histogram(*duration_series);
        auto inflight_value = shard->gauge(*inflight_series);
        auto proxy_attempts_inflight_value = shard->gauge(*proxy_attempts_inflight_series);
        auto websocket_sessions_inflight_value = shard->gauge(*websocket_sessions_inflight_series);
        if (!duration_value || !inflight_value || !proxy_attempts_inflight_value ||
            !websocket_sessions_inflight_value) {
            return false;
        }
        worker.request_duration_ = *duration_value;
        worker.inflight_ = *inflight_value;
        worker.proxy_attempts_inflight_ = *proxy_attempts_inflight_value;
        worker.websocket_sessions_inflight_ = *websocket_sessions_inflight_value;
    }
    return true;
}

AccessServerMetrics::Worker &AccessServerMetrics::worker(std::size_t index) noexcept {
    FIBER_ASSERT(valid_);
    FIBER_ASSERT(index < workers_.size());
    return workers_[index];
}

async::Task<common::IoResult<mem::IoBufChain>> AccessServerMetrics::collect(mem::IoBufNodePool &node_pool) noexcept {
    auto collected = co_await registry_.collect_text(node_pool);
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
