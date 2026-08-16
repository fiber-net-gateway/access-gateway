#include "observability/AccessProcessMetrics.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace fiber::access_server {
namespace {

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

TEST(AccessProcessMetricsTest, RendersFixedLogAndCatSnapshots) {
    AccessProcessMetricsSnapshot snapshot{
            .logging_available = true,
            .log_queue =
                    {
                            .queued_records = 2,
                            .queued_bytes = 128,
                            .peak_queued_records = 7,
                            .peak_queued_bytes = 1024,
                            .dropped_records = 3,
                            .allocation_failures = 4,
                            .formatting_failures = 5,
                            .accepting = true,
                    },
            .log_appender =
                    {
                            .written_records = 11,
                            .written_bytes = 4096,
                            .dropped_records = 6,
                            .write_errors = 7,
                            .reopen_errors = 8,
                            .rotations = 9,
                            .rotation_errors = 10,
                            .retention_errors = 12,
                            .active_file_bytes = 8192,
                    },
            .cat_enabled = true,
            .cat_state = cat::CatClientState::Running,
            .cat =
                    {
                            .queued_messages = 13,
                            .queued_bytes = 512,
                            .system_queued_messages = 2,
                            .system_queued_bytes = 64,
                            .submitted_messages = 101,
                            .sent_messages = 97,
                            .sent_bytes = 16384,
                            .dropped_queue_full = 1,
                            .dropped_unavailable = 2,
                            .dropped_sampled = 3,
                            .dropped_partial_frame = 4,
                            .encode_failures = 5,
                            .aggregation_overflow = 6,
                            .aggregate_dropped = 7,
                            .aggregate_retry_failures = 8,
                            .aggregate_encode_failures = 9,
                            .metric_overflow = 10,
                            .metric_dropped = 11,
                            .metric_retry_failures = 12,
                            .heartbeat_dropped = 13,
                            .heartbeat_encode_failures = 14,
                            .heartbeat_provider_failures = 15,
                    },
    };

    std::string output;
    append_access_process_metrics(output, snapshot);

    EXPECT_NE(output.find("access_server_log_metrics_available 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_queue_records 2"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_queue_bytes 128"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_queue_peak_records 7"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_queue_accepting 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_queue_failures_total{reason=\"queue_full\"} 3"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_queue_failures_total{reason=\"allocation\"} 4"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_appender_records_total{result=\"written\"} 11"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_appender_records_total{result=\"dropped\"} 6"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_appender_failures_total{operation=\"retention\"} 12"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_appender_rotations_total 9"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_appender_active_file_bytes 8192"), std::string::npos);

    EXPECT_NE(output.find("access_server_cat_state{state=\"running\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_state{state=\"disabled\"} 0"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_queue_messages{kind=\"all\"} 13"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_queue_messages{kind=\"system\"} 2"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_queue_bytes{kind=\"system\"} 64"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_messages_total{result=\"submitted\"} 101"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_messages_total{result=\"sent\"} 97"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_sent_bytes_total 16384"), std::string::npos);

    constexpr std::array<std::string_view, 15> kCatReasons{
            "queue_full",           "unavailable",       "sampled",           "partial_frame",    "encode",
            "aggregation_overflow", "aggregate_dropped", "aggregate_retry",   "aggregate_encode", "metric_overflow",
            "metric_dropped",       "metric_retry",      "heartbeat_dropped", "heartbeat_encode", "heartbeat_provider",
    };
    for (std::size_t i = 0; i < kCatReasons.size(); ++i) {
        const std::string series = "access_server_cat_dropped_events_total{reason=\"" + std::string(kCatReasons[i]) +
                                   "\"} " + std::to_string(i + 1);
        EXPECT_NE(output.find(series), std::string::npos) << series;
    }
    EXPECT_EQ(count_occurrences(output, "access_server_cat_dropped_events_total{reason="), 15U);
    EXPECT_EQ(output.find("appender="), std::string::npos);
    EXPECT_EQ(output.find("project="), std::string::npos);
    EXPECT_EQ(output.find("host="), std::string::npos);
}

TEST(AccessProcessMetricsTest, MissingSourcesRenderExplicitDisabledStateAndZeroes) {
    AccessProcessMetrics metrics;
    const AccessProcessMetricsSnapshot snapshot = metrics.snapshot();
    EXPECT_FALSE(snapshot.logging_available);
    EXPECT_FALSE(snapshot.cat_enabled);

    std::string output;
    metrics.append_prometheus(output);
    EXPECT_NE(output.find("access_server_log_metrics_available 0"), std::string::npos);
    EXPECT_NE(output.find("access_server_log_queue_accepting 0"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_state{state=\"disabled\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_state{state=\"created\"} 0"), std::string::npos);
    EXPECT_NE(output.find("access_server_cat_queue_messages{kind=\"all\"} 0"), std::string::npos);
}

} // namespace
} // namespace fiber::access_server
