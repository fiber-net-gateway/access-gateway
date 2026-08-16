#include "AccessProcessMetrics.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

constexpr std::array<std::string_view, 5> kCatStateLabels{
        R"({state="disabled"})", R"({state="created"})", R"({state="running"})",
        R"({state="stopping"})", R"({state="stopped"})",
};

constexpr std::array<std::string_view, 3> kLogQueueFailureLabels{
        R"({reason="queue_full"})",
        R"({reason="allocation"})",
        R"({reason="formatting"})",
};

constexpr std::array<std::string_view, 2> kLogAppenderResultLabels{
        R"({result="written"})",
        R"({result="dropped"})",
};

constexpr std::array<std::string_view, 4> kLogAppenderFailureLabels{
        R"({operation="write"})",
        R"({operation="reopen"})",
        R"({operation="rotation"})",
        R"({operation="retention"})",
};

constexpr std::array<std::string_view, 2> kCatQueueKindLabels{
        R"({kind="all"})",
        R"({kind="system"})",
};

constexpr std::array<std::string_view, 2> kCatMessageResultLabels{
        R"({result="submitted"})",
        R"({result="sent"})",
};

constexpr std::array<std::string_view, 15> kCatDropReasonLabels{
        R"({reason="queue_full"})",        R"({reason="unavailable"})",      R"({reason="sampled"})",
        R"({reason="partial_frame"})",     R"({reason="encode"})",           R"({reason="aggregation_overflow"})",
        R"({reason="aggregate_dropped"})", R"({reason="aggregate_retry"})",  R"({reason="aggregate_encode"})",
        R"({reason="metric_overflow"})",   R"({reason="metric_dropped"})",   R"({reason="metric_retry"})",
        R"({reason="heartbeat_dropped"})", R"({reason="heartbeat_encode"})", R"({reason="heartbeat_provider"})",
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

template<std::size_t N>
void append_labeled_values(std::string &output, std::string_view name, const std::array<std::string_view, N> &labels,
                           const std::array<std::uint64_t, N> &values) {
    for (std::size_t i = 0; i < N; ++i) {
        append_series(output, name, labels[i], values[i]);
    }
}

} // namespace

AccessProcessMetricsSnapshot AccessProcessMetrics::snapshot() const noexcept {
    AccessProcessMetricsSnapshot value;
    if (sources_.logger && sources_.log_appender != log::kInvalidAppenderId && sources_.logger->running()) {
        value.logging_available = true;
        value.log_queue = sources_.logger->queue_stats();
        value.log_appender = sources_.logger->appender_stats(sources_.log_appender);
    }
    if (sources_.cat_client) {
        value.cat_enabled = true;
        value.cat_state = sources_.cat_client->state();
        value.cat = sources_.cat_client->stats();
    }
    return value;
}

void AccessProcessMetrics::append_prometheus(std::string &output) const {
    append_access_process_metrics(output, snapshot());
}

void append_access_process_metrics(std::string &output, const AccessProcessMetricsSnapshot &snapshot) {
    output.reserve(output.size() + 8192);

    output.append("# HELP access_server_log_metrics_available Whether the configured Fiber logger and appender are "
                  "available.\n");
    output.append("# TYPE access_server_log_metrics_available gauge\n");
    append_series(output, "access_server_log_metrics_available", {}, snapshot.logging_available ? 1U : 0U);

    output.append("# HELP access_server_log_queue_records Records currently queued for asynchronous logging.\n");
    output.append("# TYPE access_server_log_queue_records gauge\n");
    append_series(output, "access_server_log_queue_records", {}, snapshot.log_queue.queued_records);
    output.append("# HELP access_server_log_queue_bytes Bytes currently queued for asynchronous logging.\n");
    output.append("# TYPE access_server_log_queue_bytes gauge\n");
    append_series(output, "access_server_log_queue_bytes", {}, snapshot.log_queue.queued_bytes);
    output.append("# HELP access_server_log_queue_peak_records Highest observed asynchronous log queue depth.\n");
    output.append("# TYPE access_server_log_queue_peak_records gauge\n");
    append_series(output, "access_server_log_queue_peak_records", {}, snapshot.log_queue.peak_queued_records);
    output.append("# HELP access_server_log_queue_peak_bytes Highest observed asynchronous log queue bytes.\n");
    output.append("# TYPE access_server_log_queue_peak_bytes gauge\n");
    append_series(output, "access_server_log_queue_peak_bytes", {}, snapshot.log_queue.peak_queued_bytes);
    output.append("# HELP access_server_log_queue_accepting Whether the asynchronous log queue accepts records.\n");
    output.append("# TYPE access_server_log_queue_accepting gauge\n");
    append_series(output, "access_server_log_queue_accepting", {}, snapshot.log_queue.accepting ? 1U : 0U);

    output.append("# HELP access_server_log_queue_failures_total Records lost before appender delivery.\n");
    output.append("# TYPE access_server_log_queue_failures_total counter\n");
    append_labeled_values(output, "access_server_log_queue_failures_total", kLogQueueFailureLabels,
                          std::array<std::uint64_t, kLogQueueFailureLabels.size()>{
                                  snapshot.log_queue.dropped_records, snapshot.log_queue.allocation_failures,
                                  snapshot.log_queue.formatting_failures});

    output.append("# HELP access_server_log_appender_records_total Primary appender record outcomes.\n");
    output.append("# TYPE access_server_log_appender_records_total counter\n");
    append_labeled_values(output, "access_server_log_appender_records_total", kLogAppenderResultLabels,
                          std::array<std::uint64_t, kLogAppenderResultLabels.size()>{
                                  snapshot.log_appender.written_records, snapshot.log_appender.dropped_records});
    output.append("# HELP access_server_log_appender_written_bytes_total Bytes written by the primary appender.\n");
    output.append("# TYPE access_server_log_appender_written_bytes_total counter\n");
    append_series(output, "access_server_log_appender_written_bytes_total", {}, snapshot.log_appender.written_bytes);
    output.append("# HELP access_server_log_appender_failures_total Primary appender operation failures.\n");
    output.append("# TYPE access_server_log_appender_failures_total counter\n");
    append_labeled_values(output, "access_server_log_appender_failures_total", kLogAppenderFailureLabels,
                          std::array<std::uint64_t, kLogAppenderFailureLabels.size()>{
                                  snapshot.log_appender.write_errors, snapshot.log_appender.reopen_errors,
                                  snapshot.log_appender.rotation_errors, snapshot.log_appender.retention_errors});
    output.append("# HELP access_server_log_appender_rotations_total Completed primary appender rotations.\n");
    output.append("# TYPE access_server_log_appender_rotations_total counter\n");
    append_series(output, "access_server_log_appender_rotations_total", {}, snapshot.log_appender.rotations);
    output.append("# HELP access_server_log_appender_active_file_bytes Current primary appender file size.\n");
    output.append("# TYPE access_server_log_appender_active_file_bytes gauge\n");
    append_series(output, "access_server_log_appender_active_file_bytes", {}, snapshot.log_appender.active_file_bytes);

    output.append("# HELP access_server_cat_state Configured CAT client lifecycle state.\n");
    output.append("# TYPE access_server_cat_state gauge\n");
    const std::size_t cat_state = snapshot.cat_enabled ? static_cast<std::size_t>(snapshot.cat_state) + 1U : 0U;
    FIBER_ASSERT(cat_state < kCatStateLabels.size());
    for (std::size_t state = 0; state < kCatStateLabels.size(); ++state) {
        append_series(output, "access_server_cat_state", kCatStateLabels[state], state == cat_state ? 1U : 0U);
    }

    output.append("# HELP access_server_cat_queue_messages CAT messages currently using queue budget.\n");
    output.append("# TYPE access_server_cat_queue_messages gauge\n");
    append_labeled_values(output, "access_server_cat_queue_messages", kCatQueueKindLabels,
                          std::array<std::uint64_t, kCatQueueKindLabels.size()>{snapshot.cat.queued_messages,
                                                                                snapshot.cat.system_queued_messages});
    output.append("# HELP access_server_cat_queue_bytes CAT bytes currently using queue budget.\n");
    output.append("# TYPE access_server_cat_queue_bytes gauge\n");
    append_labeled_values(output, "access_server_cat_queue_bytes", kCatQueueKindLabels,
                          std::array<std::uint64_t, kCatQueueKindLabels.size()>{snapshot.cat.queued_bytes,
                                                                                snapshot.cat.system_queued_bytes});
    output.append("# HELP access_server_cat_messages_total CAT message submission and delivery outcomes.\n");
    output.append("# TYPE access_server_cat_messages_total counter\n");
    append_labeled_values(output, "access_server_cat_messages_total", kCatMessageResultLabels,
                          std::array<std::uint64_t, kCatMessageResultLabels.size()>{snapshot.cat.submitted_messages,
                                                                                    snapshot.cat.sent_messages});
    output.append("# HELP access_server_cat_sent_bytes_total CAT bytes delivered to collectors.\n");
    output.append("# TYPE access_server_cat_sent_bytes_total counter\n");
    append_series(output, "access_server_cat_sent_bytes_total", {}, snapshot.cat.sent_bytes);

    output.append("# HELP access_server_cat_dropped_events_total Bounded CAT delivery and aggregation loss events.\n");
    output.append("# TYPE access_server_cat_dropped_events_total counter\n");
    append_labeled_values(
            output, "access_server_cat_dropped_events_total", kCatDropReasonLabels,
            std::array<std::uint64_t, kCatDropReasonLabels.size()>{
                    snapshot.cat.dropped_queue_full, snapshot.cat.dropped_unavailable, snapshot.cat.dropped_sampled,
                    snapshot.cat.dropped_partial_frame, snapshot.cat.encode_failures, snapshot.cat.aggregation_overflow,
                    snapshot.cat.aggregate_dropped, snapshot.cat.aggregate_retry_failures,
                    snapshot.cat.aggregate_encode_failures, snapshot.cat.metric_overflow, snapshot.cat.metric_dropped,
                    snapshot.cat.metric_retry_failures, snapshot.cat.heartbeat_dropped,
                    snapshot.cat.heartbeat_encode_failures, snapshot.cat.heartbeat_provider_failures});
}

} // namespace fiber::access_server
