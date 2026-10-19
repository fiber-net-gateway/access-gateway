#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_METRICS_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/prometheus/Counter.h>
#include <fiber/prometheus/DynamicCounter.h>
#include <fiber/prometheus/Gauge.h>
#include <fiber/prometheus/Histogram.h>
#include <fiber/prometheus/MetricsRegistry.h>

#include <optional>
#include <string_view>

namespace fiber::access_server {

class AccessRuntimeMetrics;

enum class AccessResponseStatus : std::uint8_t {
    Code200,
    Code201,
    Code202,
    Code204,
    Code206,
    Code301,
    Code302,
    Code303,
    Code304,
    Code307,
    Code308,
    Code400,
    Code401,
    Code403,
    Code404,
    Code405,
    Code406,
    Code408,
    Code409,
    Code410,
    Code412,
    Code413,
    Code415,
    Code416,
    Code422,
    Code426,
    Code429,
    Code499,
    Code500,
    Code501,
    Code502,
    Code503,
    Code504,
    Code505,
    Other1xx,
    Other2xx,
    Other3xx,
    Other4xx,
    Other5xx,
    Other,
    Count,
};

enum class AccessProxyExecutionResult : std::uint8_t {
    Completed,
    Failed,
    Canceled,
    Count,
};

enum class AccessProxyAttemptResult : std::uint8_t {
    Completed,
    Failed,
    Aborted,
    Count,
};

enum class AccessProxyFailurePhase : std::uint8_t {
    NoUpstreamHosts,
    UpstreamCircuitOpen,
    InvalidSelection,
    EvaluateContext,
    ResolveUpstream,
    PoolShutdown,
    Connect,
    Tls,
    BuildRequest,
    BuildHeaders,
    SendHeader,
    ReadRequestBody,
    RequestBodyTooLarge,
    SendRequestBody,
    ReadResponseHeader,
    BuildResponseHeaders,
    ResponseBodyTooLarge,
    SwitchWebSocket,
    SendResponseHeader,
    ReadResponseBody,
    WriteResponseBody,
    Count,
};

enum class AccessProxyPoolResult : std::uint8_t {
    Hit,
    Miss,
    Shutdown,
    Count,
};

enum class AccessProxyDnsResult : std::uint8_t {
    Success,
    Empty,
    Failure,
    Unavailable,
    Count,
};

enum class AccessProxyConnectResult : std::uint8_t {
    Success,
    Failure,
    TlsFailure,
    CreateFailure,
    Count,
};

enum class AccessHappyEyeballsResult : std::uint8_t {
    Success,
    Failure,
    Count,
};

enum class AccessWebSocketHandshakeResult : std::uint8_t {
    Accepted,
    Rejected,
    Failed,
    Count,
};

enum class AccessWebSocketSessionResult : std::uint8_t {
    Closed,
    Aborted,
    Count,
};

enum class AccessBodyIdleDirection : std::uint8_t {
    RequestBody,
    ResponseBody,
    Count,
};

class AccessServerMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    class Worker {
    public:
        void request_started() noexcept;
        void request_finished(const http::HttpResponseStats &response, std::chrono::microseconds duration) noexcept;
        void response_compression_selected(bool compressed) noexcept;
        void response_compression_not_acceptable() noexcept;
        void proxy_execution_finished(AccessProxyExecutionResult result) noexcept;
        void proxy_attempt_started() noexcept;
        void proxy_attempt_finished(AccessProxyAttemptResult result) noexcept;
        void proxy_failure(AccessProxyFailurePhase phase) noexcept;
        void proxy_pool_acquired(AccessProxyPoolResult result, std::uint64_t count = 1) noexcept;
        void proxy_dns_resolved(AccessProxyDnsResult result, std::uint64_t count = 1) noexcept;
        void proxy_connect_attempted(AccessProxyConnectResult result, std::uint64_t count = 1) noexcept;
        void proxy_connect_candidates_observed(std::uint64_t count) noexcept;
        void happy_eyeballs_finished(AccessHappyEyeballsResult result, std::uint64_t count = 1) noexcept;
        void websocket_handshake_finished(AccessWebSocketHandshakeResult result) noexcept;
        void websocket_session_started() noexcept;
        void websocket_session_finished(AccessWebSocketSessionResult result) noexcept;
        void proxy_body_idle_timeout(AccessBodyIdleDirection direction) noexcept;

        // Dynamic LRU series (labels known only at request time). Unlike the
        // pre-bound series above, these take a family-wide lock and may
        // allocate when a label combination is seen for the first time; a full
        // family evicts its least-recently-recorded series and restarts the
        // evicted counters from zero.
        void route_request_finished(std::string_view host_pattern, std::string_view route_path,
                                    std::string_view status_label) noexcept;
        void provider_response_received(std::string_view app, std::string_view ip_port,
                                        std::string_view status_label) noexcept;
        void proxy_transport_error(std::string_view app, std::string_view ip_port,
                                   std::string_view error_label) noexcept;
        void upstream_circuit_rejected(std::string_view app) noexcept;

    private:
        friend class AccessServerMetrics;

        static constexpr std::size_t kRequestResultCount = 4;
        static constexpr std::size_t kRequestStatusCodeCount = static_cast<std::size_t>(AccessResponseStatus::Count);
        static constexpr std::size_t kResponseCompressionResultCount = 3;
        static constexpr std::size_t kProxyExecutionResultCount =
                static_cast<std::size_t>(AccessProxyExecutionResult::Count);
        static constexpr std::size_t kProxyAttemptResultCount =
                static_cast<std::size_t>(AccessProxyAttemptResult::Count);
        static constexpr std::size_t kProxyFailurePhaseCount = static_cast<std::size_t>(AccessProxyFailurePhase::Count);
        static constexpr std::size_t kProxyPoolResultCount = static_cast<std::size_t>(AccessProxyPoolResult::Count);
        static constexpr std::size_t kProxyDnsResultCount = static_cast<std::size_t>(AccessProxyDnsResult::Count);
        static constexpr std::size_t kProxyConnectResultCount =
                static_cast<std::size_t>(AccessProxyConnectResult::Count);
        static constexpr std::size_t kHappyEyeballsResultCount =
                static_cast<std::size_t>(AccessHappyEyeballsResult::Count);
        static constexpr std::size_t kWebSocketHandshakeResultCount =
                static_cast<std::size_t>(AccessWebSocketHandshakeResult::Count);
        static constexpr std::size_t kWebSocketSessionResultCount =
                static_cast<std::size_t>(AccessWebSocketSessionResult::Count);
        static constexpr std::size_t kBodyIdleDirectionCount = static_cast<std::size_t>(AccessBodyIdleDirection::Count);

        std::array<prometheus::CounterRef, kRequestResultCount> requests_;
        std::array<prometheus::CounterRef, kRequestStatusCodeCount> requests_by_status_;
        std::array<prometheus::CounterRef, kResponseCompressionResultCount> response_compression_;
        std::array<prometheus::CounterRef, kProxyExecutionResultCount> proxy_executions_;
        std::array<prometheus::CounterRef, kProxyAttemptResultCount> proxy_attempts_;
        std::array<prometheus::CounterRef, kProxyFailurePhaseCount> proxy_failures_;
        std::array<prometheus::CounterRef, kProxyPoolResultCount> proxy_pool_acquires_;
        std::array<prometheus::CounterRef, kProxyDnsResultCount> proxy_dns_resolutions_;
        std::array<prometheus::CounterRef, kProxyConnectResultCount> proxy_connect_attempts_;
        prometheus::CounterRef proxy_connect_candidates_;
        std::array<prometheus::CounterRef, kHappyEyeballsResultCount> happy_eyeballs_;
        std::array<prometheus::CounterRef, kWebSocketHandshakeResultCount> websocket_handshakes_;
        std::array<prometheus::CounterRef, kWebSocketSessionResultCount> websocket_sessions_;
        std::array<prometheus::CounterRef, kBodyIdleDirectionCount> proxy_body_idle_timeouts_;
        // Family-wide handles shared by every worker copy; bound after freeze().
        std::optional<prometheus::DynamicCounter> route_requests_dynamic_;
        std::optional<prometheus::DynamicCounter> provider_responses_dynamic_;
        std::optional<prometheus::DynamicCounter> proxy_errors_dynamic_;
        std::optional<prometheus::DynamicCounter> circuit_rejections_dynamic_;
        prometheus::HistogramRef request_duration_;
        prometheus::GaugeRef inflight_;
        prometheus::GaugeRef proxy_attempts_inflight_;
        prometheus::GaugeRef websocket_sessions_inflight_;
    };

    explicit AccessServerMetrics(event::EventLoopGroup &workers, const AccessRuntimeMetrics *runtime_metrics = nullptr);
    ~AccessServerMetrics();

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] Worker &worker(std::size_t index) noexcept;

    [[nodiscard]] async::Task<common::IoResult<mem::IoBufChain>> collect() noexcept;

    void stop_collecting() noexcept;
    [[nodiscard]] async::Task<void> wait_for_idle() noexcept;

private:
    [[nodiscard]] bool initialize(event::EventLoopGroup &workers);

    prometheus::MetricsRegistry registry_;
    std::vector<Worker> workers_;
    const AccessRuntimeMetrics *runtime_metrics_ = nullptr;
    bool collecting_stopped_ = false;
    bool valid_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_METRICS_H
