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
#include <fiber/prometheus/Gauge.h>
#include <fiber/prometheus/Histogram.h>
#include <fiber/prometheus/MetricsRegistry.h>

namespace fiber::access_server {

class AccessRuntimeMetrics;

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

    private:
        friend class AccessServerMetrics;

        static constexpr std::size_t kRequestResultCount = 4;
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

        std::array<prometheus::CounterRef, kRequestResultCount> requests_;
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
        prometheus::HistogramRef request_duration_;
        prometheus::GaugeRef inflight_;
        prometheus::GaugeRef proxy_attempts_inflight_;
        prometheus::GaugeRef websocket_sessions_inflight_;
    };

    explicit AccessServerMetrics(event::EventLoopGroup &workers, const AccessRuntimeMetrics *runtime_metrics = nullptr);
    ~AccessServerMetrics();

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] Worker &worker(std::size_t index) noexcept;

    [[nodiscard]] async::Task<common::IoResult<mem::IoBufChain>> collect(mem::IoBufNodePool &node_pool) noexcept;

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
