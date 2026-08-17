#include "AccessUpstreamCircuit.h"

#include <algorithm>
#include <limits>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

AccessUpstreamCircuit::AccessUpstreamCircuit(std::size_t max_fails, std::chrono::milliseconds fail_timeout) noexcept :
    max_fails_(max_fails),
    fail_timeout_nanoseconds_(std::chrono::duration_cast<std::chrono::nanoseconds>(fail_timeout).count()) {
    FIBER_ASSERT(fail_timeout_nanoseconds_ > 0);
}

bool AccessUpstreamCircuit::available(TimePoint now) const noexcept {
    if (max_fails_ == 0 || failures_.load(std::memory_order_acquire) < max_fails_) {
        return true;
    }
    return timestamp(now) > next_probe_at_nanoseconds_.load(std::memory_order_acquire);
}

std::optional<AccessUpstreamCircuit::Permit> AccessUpstreamCircuit::acquire(TimePoint now) noexcept {
    if (max_fails_ == 0) {
        return Permit{};
    }

    const std::size_t failures = failures_.load(std::memory_order_acquire);
    if (failures == 0) {
        return Permit{};
    }

    const std::int64_t now_nanoseconds = timestamp(now);
    std::int64_t next_probe = next_probe_at_nanoseconds_.load(std::memory_order_acquire);
    if (now_nanoseconds <= next_probe) {
        return failures < max_fails_ ? std::optional<Permit>(Permit{}) : std::nullopt;
    }

    const std::uint64_t recovery_epoch = failure_epoch_.load(std::memory_order_acquire);
    const std::int64_t deadline = next_probe_deadline(now_nanoseconds);
    if (next_probe_at_nanoseconds_.compare_exchange_strong(next_probe, deadline, std::memory_order_acq_rel,
                                                           std::memory_order_acquire)) {
        return Permit{.recovery_epoch = recovery_epoch};
    }
    return failures < max_fails_ ? std::optional<Permit>(Permit{}) : std::nullopt;
}

void AccessUpstreamCircuit::report(Permit permit, bool success, TimePoint now) noexcept {
    if (max_fails_ == 0) {
        return;
    }

    if (success) {
        if (permit.recovery_epoch == 0) {
            return;
        }
        std::lock_guard guard(report_mutex_);
        if (failure_epoch_.load(std::memory_order_relaxed) == permit.recovery_epoch) {
            failures_.store(0, std::memory_order_release);
        }
        return;
    }

    std::lock_guard guard(report_mutex_);
    const std::uint64_t epoch = failure_epoch_.load(std::memory_order_relaxed);
    FIBER_ASSERT(epoch != std::numeric_limits<std::uint64_t>::max());
    failure_epoch_.store(epoch + 1, std::memory_order_release);
    postpone_probe(next_probe_deadline(timestamp(now)));
    const std::size_t failures = failures_.load(std::memory_order_relaxed);
    if (failures != std::numeric_limits<std::size_t>::max()) {
        failures_.store(failures + 1, std::memory_order_release);
    }
}

std::int64_t AccessUpstreamCircuit::timestamp(TimePoint value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

std::int64_t AccessUpstreamCircuit::next_probe_deadline(std::int64_t now) const noexcept {
    if (now > std::numeric_limits<std::int64_t>::max() - fail_timeout_nanoseconds_) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return now + fail_timeout_nanoseconds_;
}

void AccessUpstreamCircuit::postpone_probe(std::int64_t deadline) noexcept {
    std::int64_t current = next_probe_at_nanoseconds_.load(std::memory_order_relaxed);
    while (current < deadline && !next_probe_at_nanoseconds_.compare_exchange_weak(
                                         current, deadline, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

} // namespace fiber::access_server
