#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_METRICS_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::event {
class EventLoop;
}

namespace fiber::access_server {

class AccessRouteSnapshot;

// Every value maps to one predeclared Prometheus series. No configuration or
// request value can become a label through this interface.
enum class AccessConfigMetricEvent : std::uint8_t {
    ProjectListAccepted,
    ProjectListSubscriptionFailed,
    ProjectListDecodeFailed,
    ProjectRouteIgnoredEmpty,
    ProjectRouteVersionUnchanged,
    ProjectRoutePublished,
    ProjectRouteUnloaded,
    ProjectRouteRemoved,
    ProjectRouteSubscriptionFailed,
    ProjectRouteDecodeFailed,
    ProjectRouteCompileFailed,
    ProjectRouteServiceReadyFailed,
    ProjectRoutePublishFailed,
    Count,
};

enum class AccessConfigMetricReadinessState : std::uint8_t {
    WaitingForProjectList,
    SynchronizingProjects,
    Ready,
    Unavailable,
    Stopped,
};

enum class AccessConfigMetricStage : std::uint8_t {
    ProjectCompile,
    ServiceReady,
    GlobalBuild,
    Publish,
    Count,
};

struct AccessConfigMetricReadiness {
    AccessConfigMetricReadinessState state = AccessConfigMetricReadinessState::WaitingForProjectList;
    std::uint64_t desired_projects = 0;
    std::uint64_t subscribed_projects = 0;
    std::uint64_t synchronized_projects = 0;
    std::uint64_t retrying_projects = 0;
    std::uint64_t processing_projects = 0;
    std::uint64_t ready_to_publish_projects = 0;
    std::uint64_t rejected_projects = 0;
};

struct AccessConfigMetricsObserver {
    using EventFunction = void (*)(void *context, AccessConfigMetricEvent event) noexcept;
    using ReadinessFunction = void (*)(void *context, const AccessConfigMetricReadiness &readiness) noexcept;
    using SnapshotFunction = void (*)(void *context, const AccessRouteSnapshot &snapshot) noexcept;
    using DurationFunction = void (*)(void *context, AccessConfigMetricStage stage,
                                      std::chrono::nanoseconds duration) noexcept;

    void *context = nullptr;
    EventFunction on_event = nullptr;
    ReadinessFunction on_readiness = nullptr;
    SnapshotFunction on_snapshot = nullptr;
    DurationFunction on_duration = nullptr;
};

// The Nacos EventLoop is the sole writer. Metrics workers take lock-free,
// bounded snapshots and render only fixed aggregate series.
class AccessConfigMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessConfigMetrics(event::EventLoop &owner) noexcept;

    [[nodiscard]] AccessConfigMetricsObserver observer() noexcept;
    void append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const;

private:
    static constexpr std::size_t kEventCount = static_cast<std::size_t>(AccessConfigMetricEvent::Count);
    static constexpr std::size_t kStageCount = static_cast<std::size_t>(AccessConfigMetricStage::Count);

    struct Snapshot {
        AccessConfigMetricReadiness readiness;
        std::uint64_t generation = 0;
        std::uint64_t projects = 0;
        std::uint64_t hosts = 0;
        std::uint64_t routes = 0;
        std::uint64_t compiled_programs = 0;
        std::uint64_t estimated_bytes = 0;
        std::uint64_t static_response_bytes = 0;
        std::int64_t published_at_nanoseconds = 0;
    };

    static void observe_event(void *context, AccessConfigMetricEvent event) noexcept;
    static void observe_readiness(void *context, const AccessConfigMetricReadiness &readiness) noexcept;
    static void observe_snapshot(void *context, const AccessRouteSnapshot &snapshot) noexcept;
    static void observe_duration(void *context, AccessConfigMetricStage stage,
                                 std::chrono::nanoseconds duration) noexcept;

    void record_event(AccessConfigMetricEvent event) noexcept;
    void record_duration(AccessConfigMetricStage stage, std::chrono::nanoseconds duration) noexcept;
    void update_readiness(const AccessConfigMetricReadiness &readiness) noexcept;
    void update_snapshot(const AccessRouteSnapshot &snapshot) noexcept;
    void begin_update() noexcept;
    void finish_update() noexcept;
    [[nodiscard]] Snapshot load_snapshot() const noexcept;

    event::EventLoop *owner_ = nullptr;
    std::array<std::atomic<std::uint64_t>, kEventCount> events_{};
    std::array<std::atomic<std::uint64_t>, kStageCount> duration_observations_{};
    std::array<std::atomic<std::uint64_t>, kStageCount> duration_nanoseconds_{};
    std::array<std::atomic<std::uint64_t>, kStageCount> duration_max_nanoseconds_{};
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint8_t> readiness_state_{
            static_cast<std::uint8_t>(AccessConfigMetricReadinessState::WaitingForProjectList)};
    std::atomic<std::uint64_t> desired_projects_{0};
    std::atomic<std::uint64_t> subscribed_projects_{0};
    std::atomic<std::uint64_t> synchronized_projects_{0};
    std::atomic<std::uint64_t> retrying_projects_{0};
    std::atomic<std::uint64_t> processing_projects_{0};
    std::atomic<std::uint64_t> ready_to_publish_projects_{0};
    std::atomic<std::uint64_t> rejected_projects_{0};
    std::atomic<std::uint64_t> snapshot_generation_{0};
    std::atomic<std::uint64_t> snapshot_projects_{0};
    std::atomic<std::uint64_t> snapshot_hosts_{0};
    std::atomic<std::uint64_t> snapshot_routes_{0};
    std::atomic<std::uint64_t> snapshot_compiled_programs_{0};
    std::atomic<std::uint64_t> snapshot_estimated_bytes_{0};
    std::atomic<std::uint64_t> snapshot_static_response_bytes_{0};
    std::atomic<std::int64_t> snapshot_published_at_nanoseconds_{0};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_METRICS_H
