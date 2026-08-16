#include "../src/observability/AccessTlsMetrics.h"

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <latch>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

std::optional<std::uint64_t> tls_metric_value(std::string_view text, std::string_view series) {
    std::size_t position = 0;
    for (;;) {
        position = text.find(series, position);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        if (position == 0 || text[position - 1] == '\n') {
            break;
        }
        position += series.size();
    }
    const std::size_t begin = position + series.size();
    const std::size_t end = text.find('\n', begin);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto converted = std::from_chars(text.data() + begin, text.data() + end, value);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + end) {
        return std::nullopt;
    }
    return value;
}

TEST(AccessTlsMetricsTest, RendersBoundedRotationReclaimAndRetentionMetrics) {
    using namespace std::chrono_literals;

    event::EventLoop loop;
    AccessTlsMetrics metrics(loop);
    const AccessTlsMetricsObserver observer = metrics.observer();
    std::string retained_output;
    std::string reclaimed_output;

    async::spawn(loop, [&]() -> async::DetachedTask {
        const auto now = event::EventLoop::current().now();
        observer.record_rotation();
        observer.record_rotation();
        observer.record_reclaim(AccessTlsReclaimObservation{
                .trigger = AccessTlsReclaimTrigger::Publication,
                .retired_snapshots = 2,
                .oldest_retired_at = now - 5s,
        });
        metrics.append_prometheus(retained_output, now);

        observer.record_reclaim(AccessTlsReclaimObservation{
                .trigger = AccessTlsReclaimTrigger::HazardClear,
                .reclaimed_snapshots = 2,
                .max_reclaimed_retention = 7s,
        });
        metrics.append_prometheus(reclaimed_output, now);
        loop.stop();
        co_return;
    });
    loop.run();

    EXPECT_NE(retained_output.find("access_server_tls_certificate_rotations_total 2"), std::string::npos);
    EXPECT_NE(retained_output.find("access_server_tls_certificate_reclaim_runs_total{trigger=\"publish\"} 1"),
              std::string::npos);
    EXPECT_NE(retained_output.find("access_server_tls_certificate_reclaimed_snapshots_total{trigger=\"publish\"} 0"),
              std::string::npos);
    EXPECT_NE(retained_output.find("access_server_tls_certificate_retired_snapshots 2"), std::string::npos);
    EXPECT_NE(retained_output.find("access_server_tls_certificate_oldest_retired_age_seconds 5"), std::string::npos);
    EXPECT_NE(retained_output.find("access_server_tls_certificate_max_retention_seconds 5"), std::string::npos);

    EXPECT_NE(reclaimed_output.find("access_server_tls_certificate_reclaim_runs_total{trigger=\"hazard_clear\"} 1"),
              std::string::npos);
    EXPECT_NE(reclaimed_output.find(
                      "access_server_tls_certificate_reclaimed_snapshots_total{trigger=\"hazard_clear\"} 2"),
              std::string::npos);
    EXPECT_NE(reclaimed_output.find("access_server_tls_certificate_retired_snapshots 0"), std::string::npos);
    EXPECT_NE(reclaimed_output.find("access_server_tls_certificate_oldest_retired_age_seconds 0"), std::string::npos);
    EXPECT_NE(reclaimed_output.find("access_server_tls_certificate_max_retention_seconds 7"), std::string::npos);
    EXPECT_EQ(reclaimed_output.find("certificate-id"), std::string::npos);
}

TEST(AccessTlsMetricsTest, ReadersNeverObserveTornRetirementSamples) {
    using namespace std::chrono_literals;

    event::EventLoop loop;
    AccessTlsMetrics metrics(loop);
    const AccessTlsMetricsObserver observer = metrics.observer();
    const auto sample_now = std::chrono::steady_clock::now();
    std::atomic<bool> done = false;
    std::atomic<bool> inconsistent = false;
    std::atomic<std::uint64_t> reads = 0;
    std::latch reader_started(1);

    std::thread reader([&]() {
        bool first = true;
        do {
            std::string output;
            metrics.append_prometheus(output, sample_now);
            const auto retired = tls_metric_value(output, "access_server_tls_certificate_retired_snapshots ");
            const auto age = tls_metric_value(output, "access_server_tls_certificate_oldest_retired_age_seconds ");
            const bool invalid =
                    !retired || !age || (*retired == 0 && *age != 0) || (*retired == 1 && *age != 2) || *retired > 1;
            if (first) {
                first = false;
                reader_started.count_down();
            }
            if (invalid) {
                inconsistent.store(true, std::memory_order_release);
                break;
            }
            reads.fetch_add(1, std::memory_order_relaxed);
        } while (!done.load(std::memory_order_acquire));
    });
    reader_started.wait();

    async::spawn(loop, [&]() -> async::DetachedTask {
        for (std::uint64_t value = 0; value < 10000; ++value) {
            const bool retained = (value & 1U) != 0;
            observer.record_reclaim(AccessTlsReclaimObservation{
                    .trigger = AccessTlsReclaimTrigger::Publication,
                    .retired_snapshots = retained ? 1U : 0U,
                    .oldest_retired_at = retained ? sample_now - 2s : std::chrono::steady_clock::time_point{},
            });
        }
        done.store(true, std::memory_order_release);
        loop.stop();
        co_return;
    });
    loop.run();
    reader.join();

    EXPECT_FALSE(inconsistent.load(std::memory_order_acquire));
    EXPECT_GT(reads.load(std::memory_order_relaxed), 0u);
}

} // namespace
} // namespace fiber::access_server
