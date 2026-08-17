#ifndef FIBER_ACCESS_SERVER_ACCESS_UPSTREAM_CIRCUIT_H
#define FIBER_ACCESS_SERVER_ACCESS_UPSTREAM_CIRCUIT_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

// Service-worker SWRR shards share this bounded failure state. Healthy selects
// only read atomics; failure/recovery reports serialize per endpoint rather
// than through the cluster-wide selection mutex.
class AccessUpstreamCircuit final : public common::NonCopyable, public common::NonMovable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Permit {
        std::uint64_t recovery_epoch = 0;
    };

    AccessUpstreamCircuit(std::size_t max_fails, std::chrono::milliseconds fail_timeout) noexcept;

    [[nodiscard]] bool available(TimePoint now) const noexcept;
    [[nodiscard]] std::optional<Permit> acquire(TimePoint now) noexcept;
    void report(Permit permit, bool success, TimePoint now) noexcept;

    [[nodiscard]] std::size_t failure_count() const noexcept { return failures_.load(std::memory_order_acquire); }

private:
    [[nodiscard]] static std::int64_t timestamp(TimePoint value) noexcept;
    [[nodiscard]] std::int64_t next_probe_deadline(std::int64_t now) const noexcept;
    void postpone_probe(std::int64_t deadline) noexcept;

    const std::size_t max_fails_;
    const std::int64_t fail_timeout_nanoseconds_;
    std::atomic<std::size_t> failures_{0};
    std::atomic<std::int64_t> next_probe_at_nanoseconds_{0};
    std::atomic<std::uint64_t> failure_epoch_{0};
    std::mutex report_mutex_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_UPSTREAM_CIRCUIT_H
