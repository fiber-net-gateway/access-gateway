#include "AccessTlsMetrics.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessTlsReclaimTrigger::Count)> kTriggerLabels{
        R"({trigger="publish"})",
        R"({trigger="hazard_clear"})",
        R"({trigger="shutdown"})",
};

void append_unsigned(std::string &output, std::uint64_t value) {
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    FIBER_ASSERT(converted.ec == std::errc{});
    output.append(buffer.data(), converted.ptr);
}

void append_series(std::string &output, std::string_view name, std::string_view labels, std::uint64_t value) {
    output.append(name);
    output.append(labels);
    output.push_back(' ');
    append_unsigned(output, value);
    output.push_back('\n');
}

std::int64_t steady_nanoseconds(std::chrono::steady_clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

std::uint64_t nonnegative_nanoseconds(std::chrono::nanoseconds value) noexcept {
    return value.count() > 0 ? static_cast<std::uint64_t>(value.count()) : 0;
}

} // namespace

AccessTlsMetrics::AccessTlsMetrics(event::EventLoop &owner) noexcept : owner_(&owner) {}

AccessTlsMetricsObserver AccessTlsMetrics::observer() noexcept {
    return AccessTlsMetricsObserver{
            .context = this,
            .on_rotation = &observe_rotation,
            .on_reclaim = &observe_reclaim,
    };
}

void AccessTlsMetrics::observe_rotation(void *context) noexcept {
    static_cast<AccessTlsMetrics *>(context)->record_rotation();
}

void AccessTlsMetrics::observe_reclaim(void *context, const AccessTlsReclaimObservation &observation) noexcept {
    static_cast<AccessTlsMetrics *>(context)->record_reclaim(observation);
}

void AccessTlsMetrics::record_rotation() noexcept {
    FIBER_ASSERT(owner_->in_loop());
    const std::uint64_t previous = rotations_.fetch_add(1, std::memory_order_relaxed);
    FIBER_ASSERT(previous != std::numeric_limits<std::uint64_t>::max());
}

void AccessTlsMetrics::record_reclaim(const AccessTlsReclaimObservation &observation) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    const std::size_t trigger = static_cast<std::size_t>(observation.trigger);
    FIBER_ASSERT(trigger < reclaim_runs_.size());
    const std::uint64_t previous_runs = reclaim_runs_[trigger].fetch_add(1, std::memory_order_relaxed);
    FIBER_ASSERT(previous_runs != std::numeric_limits<std::uint64_t>::max());
    const std::uint64_t previous_reclaimed =
            reclaimed_snapshots_[trigger].fetch_add(observation.reclaimed_snapshots, std::memory_order_relaxed);
    FIBER_ASSERT(previous_reclaimed <= std::numeric_limits<std::uint64_t>::max() - observation.reclaimed_snapshots);

    const std::uint64_t reclaimed_retention = nonnegative_nanoseconds(observation.max_reclaimed_retention);
    writer_max_retention_nanoseconds_ = std::max(writer_max_retention_nanoseconds_, reclaimed_retention);
    begin_update();
    retired_snapshots_.store(observation.retired_snapshots, std::memory_order_relaxed);
    oldest_retired_at_nanoseconds_.store(
            observation.retired_snapshots == 0 ? 0 : steady_nanoseconds(observation.oldest_retired_at),
            std::memory_order_relaxed);
    max_retention_nanoseconds_.store(writer_max_retention_nanoseconds_, std::memory_order_relaxed);
    finish_update();
}

void AccessTlsMetrics::begin_update() noexcept {
    const std::uint64_t previous = sequence_.fetch_add(1, std::memory_order_acq_rel);
    FIBER_ASSERT((previous & 1U) == 0);
}

void AccessTlsMetrics::finish_update() noexcept {
    const std::uint64_t previous = sequence_.fetch_add(1, std::memory_order_release);
    FIBER_ASSERT((previous & 1U) != 0);
}

AccessTlsMetrics::Snapshot AccessTlsMetrics::load_snapshot() const noexcept {
    Snapshot snapshot;
    for (;;) {
        const std::uint64_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        snapshot.retired_snapshots = retired_snapshots_.load(std::memory_order_relaxed);
        snapshot.oldest_retired_at_nanoseconds = oldest_retired_at_nanoseconds_.load(std::memory_order_relaxed);
        snapshot.max_retention_nanoseconds = max_retention_nanoseconds_.load(std::memory_order_relaxed);
        if (sequence_.load(std::memory_order_acquire) == before) {
            return snapshot;
        }
    }
}

void AccessTlsMetrics::append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const {
    const Snapshot snapshot = load_snapshot();
    output.reserve(output.size() + 2048);

    output.append("# HELP access_server_tls_certificate_rotations_total Published TLS certificate rotations.\n");
    output.append("# TYPE access_server_tls_certificate_rotations_total counter\n");
    append_series(output, "access_server_tls_certificate_rotations_total", {},
                  rotations_.load(std::memory_order_relaxed));

    output.append("# HELP access_server_tls_certificate_reclaim_runs_total TLS snapshot reclaim scans.\n");
    output.append("# TYPE access_server_tls_certificate_reclaim_runs_total counter\n");
    output.append("# HELP access_server_tls_certificate_reclaimed_snapshots_total Reclaimed TLS snapshots.\n");
    output.append("# TYPE access_server_tls_certificate_reclaimed_snapshots_total counter\n");
    for (std::size_t i = 0; i < kTriggerLabels.size(); ++i) {
        append_series(output, "access_server_tls_certificate_reclaim_runs_total", kTriggerLabels[i],
                      reclaim_runs_[i].load(std::memory_order_relaxed));
        append_series(output, "access_server_tls_certificate_reclaimed_snapshots_total", kTriggerLabels[i],
                      reclaimed_snapshots_[i].load(std::memory_order_relaxed));
    }

    output.append("# HELP access_server_tls_certificate_retired_snapshots TLS snapshots awaiting hazard release.\n");
    output.append("# TYPE access_server_tls_certificate_retired_snapshots gauge\n");
    append_series(output, "access_server_tls_certificate_retired_snapshots", {}, snapshot.retired_snapshots);

    const std::int64_t now_nanoseconds = steady_nanoseconds(now);
    std::uint64_t oldest_age_nanoseconds = 0;
    if (snapshot.retired_snapshots != 0 && now_nanoseconds > snapshot.oldest_retired_at_nanoseconds) {
        oldest_age_nanoseconds = static_cast<std::uint64_t>(now_nanoseconds - snapshot.oldest_retired_at_nanoseconds);
    }
    const std::uint64_t max_retention_nanoseconds =
            std::max(snapshot.max_retention_nanoseconds, oldest_age_nanoseconds);
    output.append("# HELP access_server_tls_certificate_oldest_retired_age_seconds Age of the oldest retained TLS "
                  "snapshot.\n");
    output.append("# TYPE access_server_tls_certificate_oldest_retired_age_seconds gauge\n");
    append_series(output, "access_server_tls_certificate_oldest_retired_age_seconds", {},
                  oldest_age_nanoseconds / 1000000000U);
    output.append("# HELP access_server_tls_certificate_max_retention_seconds Longest observed TLS snapshot "
                  "retention.\n");
    output.append("# TYPE access_server_tls_certificate_max_retention_seconds gauge\n");
    append_series(output, "access_server_tls_certificate_max_retention_seconds", {},
                  max_retention_nanoseconds / 1000000000U);
}

} // namespace fiber::access_server
