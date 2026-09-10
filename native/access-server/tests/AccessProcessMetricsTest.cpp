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

TEST(AccessProcessMetricsTest, ParsesProcStatWithPlainAndHostileComm) {
    constexpr std::string_view kPlain =
            "4242 (access-server) R 1 4242 4242 0 -1 4194560 12345 678 0 0 900 350 0 0 20 0 16 0 812345 0";
    detail::AccessProcStatFields fields{};
    ASSERT_TRUE(detail::parse_access_proc_stat(kPlain, fields));
    EXPECT_EQ(fields.utime_ticks, 900U);
    EXPECT_EQ(fields.stime_ticks, 350U);
    EXPECT_EQ(fields.threads, 16U);
    EXPECT_EQ(fields.starttime_ticks, 812345U);

    constexpr std::string_view kHostileComm =
            "4242 (worker (2) test) S 1 4242 4242 0 -1 4194560 1 2 0 0 7 3 0 0 20 0 4 0 99 0";
    ASSERT_TRUE(detail::parse_access_proc_stat(kHostileComm, fields));
    EXPECT_EQ(fields.utime_ticks, 7U);
    EXPECT_EQ(fields.stime_ticks, 3U);
    EXPECT_EQ(fields.threads, 4U);
    EXPECT_EQ(fields.starttime_ticks, 99U);

    EXPECT_FALSE(detail::parse_access_proc_stat("4242 (access-server", fields));
    EXPECT_FALSE(detail::parse_access_proc_stat("4242 (access-server) R 1 2 3 4 5 6 7 8 9 10 11", fields));
    EXPECT_FALSE(detail::parse_access_proc_stat(
            "4242 (access-server) R 1 2 3 4 5 6 7 8 9 10 not-a-number 0 0 20 0 16 0 99 0", fields));
}

TEST(AccessProcessMetricsTest, ParsesProcStatusFields) {
    constexpr std::string_view kStatus =
            "Name:\taccess-server\nUmask:\t0022\nState:\tR (running)\nVmPeak:\t  204800 kB\nVmSize:\t  102400 "
            "kB\nVmRSS:\t   51200 kB\nThreads:\t16\nCpus_allowed_list:\t0-3\n";
    detail::AccessProcStatusFields fields{};
    ASSERT_TRUE(detail::parse_access_proc_status(kStatus, fields));
    EXPECT_EQ(fields.resident_kb, 51200U);
    EXPECT_EQ(fields.virtual_kb, 102400U);
    EXPECT_EQ(fields.threads, 16U);

    EXPECT_FALSE(detail::parse_access_proc_status("Name:\taccess-server\nVmSize:\t  102400 kB\n", fields));
    EXPECT_FALSE(detail::parse_access_proc_status("", fields));
    EXPECT_TRUE(detail::parse_access_proc_status("garbage line without colons\nVmRSS:\t   1 kB\nVmSize:\t   1 "
                                                 "kB\nThreads:\t2\n",
                                                 fields));
    EXPECT_FALSE(detail::parse_access_proc_status("VmRSS:\t   x kB\nVmSize:\t   1 kB\nThreads:\t2\n", fields));
}

TEST(AccessProcessMetricsTest, RendersProcessResourceSnapshot) {
    AccessProcessMetricsSnapshot snapshot;
    snapshot.process.available = true;
    snapshot.process.cpu_milliseconds = 1234567;
    snapshot.process.resident_bytes = 52428800;
    snapshot.process.virtual_bytes = 104857600;
    snapshot.process.threads = 16;
    snapshot.process.open_fds = 42;
    snapshot.process.max_fds = 1024;
    snapshot.process.start_time_milliseconds = 1789019874555;

    std::string output;
    append_access_process_metrics(output, snapshot);

    EXPECT_NE(output.find("access_server_process_metrics_available 1"), std::string::npos);
    EXPECT_NE(output.find("# TYPE access_server_process_cpu_seconds_total counter"), std::string::npos);
    EXPECT_NE(output.find("access_server_process_cpu_seconds_total 1234.567"), std::string::npos);
    EXPECT_NE(output.find("access_server_process_resident_memory_bytes 52428800"), std::string::npos);
    EXPECT_NE(output.find("access_server_process_virtual_memory_bytes 104857600"), std::string::npos);
    EXPECT_NE(output.find("access_server_process_threads 16"), std::string::npos);
    EXPECT_NE(output.find("access_server_process_open_fds 42"), std::string::npos);
    EXPECT_NE(output.find("access_server_process_max_fds 1024"), std::string::npos);
    EXPECT_NE(output.find("access_server_process_start_time_seconds 1789019874.555"), std::string::npos);
}

TEST(AccessProcessMetricsTest, UnavailableProcessResourcesRenderFlagOnly) {
    AccessProcessMetricsSnapshot snapshot;
    snapshot.process.available = false;

    std::string output;
    append_access_process_metrics(output, snapshot);

    EXPECT_NE(output.find("access_server_process_metrics_available 0"), std::string::npos);
    EXPECT_EQ(output.find("access_server_process_cpu_seconds_total "), std::string::npos);
    EXPECT_EQ(output.find("access_server_process_resident_memory_bytes "), std::string::npos);
    EXPECT_EQ(output.find("access_server_process_threads "), std::string::npos);
}

TEST(AccessProcessMetricsTest, CollectsLiveProcessResources) {
    const AccessProcessResourceStats first = detail::collect_access_process_resources();
    ASSERT_TRUE(first.available);
    EXPECT_GT(first.resident_bytes, 0U);
    EXPECT_GT(first.max_fds, 0U);
    EXPECT_GT(first.threads, 0U);
    EXPECT_GT(first.start_time_milliseconds, 1000000000ULL);

    const AccessProcessResourceStats second = detail::collect_access_process_resources();
    ASSERT_TRUE(second.available);
    EXPECT_GE(second.cpu_milliseconds, first.cpu_milliseconds);
    EXPECT_GE(second.start_time_milliseconds + 60000U, first.start_time_milliseconds);
}

} // namespace
} // namespace fiber::access_server
