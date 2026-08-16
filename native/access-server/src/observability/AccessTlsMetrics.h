#ifndef FIBER_ACCESS_SERVER_ACCESS_TLS_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_TLS_METRICS_H

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

enum class AccessTlsReclaimTrigger : std::uint8_t {
    Publication,
    HazardClear,
    Shutdown,
    Count,
};

struct AccessTlsReclaimObservation {
    AccessTlsReclaimTrigger trigger = AccessTlsReclaimTrigger::Publication;
    std::uint64_t reclaimed_snapshots = 0;
    std::uint64_t retired_snapshots = 0;
    std::chrono::steady_clock::time_point oldest_retired_at{};
    std::chrono::nanoseconds max_reclaimed_retention{0};
};

struct AccessTlsMetricsObserver {
    using RotationFunction = void (*)(void *context) noexcept;
    using ReclaimFunction = void (*)(void *context, const AccessTlsReclaimObservation &observation) noexcept;

    void record_rotation() const noexcept {
        if (on_rotation != nullptr) {
            on_rotation(context);
        }
    }

    void record_reclaim(const AccessTlsReclaimObservation &observation) const noexcept {
        if (on_reclaim != nullptr) {
            on_reclaim(context, observation);
        }
    }

    void *context = nullptr;
    RotationFunction on_rotation = nullptr;
    ReclaimFunction on_reclaim = nullptr;
};

// The Nacos EventLoop is the sole writer. Metrics workers take a lock-free,
// bounded snapshot; certificate identities and configuration values never
// become labels.
class AccessTlsMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessTlsMetrics(event::EventLoop &owner) noexcept;

    [[nodiscard]] AccessTlsMetricsObserver observer() noexcept;
    void append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const;

private:
    static constexpr std::size_t kTriggerCount = static_cast<std::size_t>(AccessTlsReclaimTrigger::Count);

    struct Snapshot {
        std::uint64_t retired_snapshots = 0;
        std::int64_t oldest_retired_at_nanoseconds = 0;
        std::uint64_t max_retention_nanoseconds = 0;
    };

    static void observe_rotation(void *context) noexcept;
    static void observe_reclaim(void *context, const AccessTlsReclaimObservation &observation) noexcept;

    void record_rotation() noexcept;
    void record_reclaim(const AccessTlsReclaimObservation &observation) noexcept;
    void begin_update() noexcept;
    void finish_update() noexcept;
    [[nodiscard]] Snapshot load_snapshot() const noexcept;

    event::EventLoop *owner_ = nullptr;
    std::atomic<std::uint64_t> rotations_{0};
    std::array<std::atomic<std::uint64_t>, kTriggerCount> reclaim_runs_{};
    std::array<std::atomic<std::uint64_t>, kTriggerCount> reclaimed_snapshots_{};
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> retired_snapshots_{0};
    std::atomic<std::int64_t> oldest_retired_at_nanoseconds_{0};
    std::atomic<std::uint64_t> max_retention_nanoseconds_{0};
    std::uint64_t writer_max_retention_nanoseconds_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_TLS_METRICS_H
