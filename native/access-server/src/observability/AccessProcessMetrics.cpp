#include "AccessProcessMetrics.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

#include <dirent.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

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

void append_seconds(std::string &output, std::uint64_t milliseconds) {
    append_unsigned(output, milliseconds / 1000U);
    output.push_back('.');
    const std::uint32_t fraction = static_cast<std::uint32_t>(milliseconds % 1000U);
    output.push_back(static_cast<char>('0' + fraction / 100U));
    output.push_back(static_cast<char>('0' + (fraction / 10U) % 10U));
    output.push_back(static_cast<char>('0' + fraction % 10U));
}

void append_seconds_series(std::string &output, std::string_view name, std::string_view labels,
                           std::uint64_t milliseconds) {
    output.append(name);
    output.append(labels);
    output.push_back(' ');
    append_seconds(output, milliseconds);
    output.push_back('\n');
}

bool next_token(std::string_view text, std::size_t &position, std::string_view &token) noexcept {
    while (position < text.size() &&
           (text[position] == ' ' || text[position] == '\t' || text[position] == '\n' || text[position] == '\r')) {
        ++position;
    }
    if (position == text.size()) {
        return false;
    }
    const std::size_t begin = position;
    while (position < text.size() && text[position] != ' ' && text[position] != '\t' && text[position] != '\n' &&
           text[position] != '\r') {
        ++position;
    }
    token = text.substr(begin, position - begin);
    return true;
}

bool parse_uint_token(std::string_view token, std::uint64_t &value) noexcept {
    if (token.empty()) {
        return false;
    }
    const auto converted = std::from_chars(token.data(), token.data() + token.size(), value);
    return converted.ec == std::errc{} && converted.ptr == token.data() + token.size();
}

bool read_small_file(const char *path, std::array<char, 8192> &storage, std::string_view &text) noexcept {
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t read_bytes = std::fread(storage.data(), 1, storage.size(), file);
    std::fclose(file);
    text = std::string_view(storage.data(), read_bytes);
    return read_bytes > 0;
}

bool count_open_fds(std::uint64_t &count) noexcept {
    DIR *directory = opendir("/proc/self/fd");
    if (directory == nullptr) {
        return false;
    }
    std::uint64_t entries = 0;
    while (const dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        ++entries;
    }
    closedir(directory);
    // The directory handle itself shows up in its own listing.
    count = entries > 0 ? entries - 1 : 0;
    return true;
}

bool resolve_max_fds(std::uint64_t &max_fds) noexcept {
    rlimit limits{};
    if (getrlimit(RLIMIT_NOFILE, &limits) != 0) {
        return false;
    }
    if (limits.rlim_cur != RLIM_INFINITY) {
        max_fds = limits.rlim_cur;
        return true;
    }
    max_fds = limits.rlim_max != RLIM_INFINITY ? limits.rlim_max : std::numeric_limits<std::uint64_t>::max();
    return true;
}

bool resolve_start_time_milliseconds(std::uint64_t starttime_ticks, long clock_ticks,
                                     std::uint64_t &start_time_milliseconds) noexcept {
    if (clock_ticks <= 0 || starttime_ticks > std::numeric_limits<std::uint64_t>::max() / 1000U) {
        return false;
    }
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return false;
    }
    struct sysinfo info{};
    if (sysinfo(&info) != 0) {
        return false;
    }
    const std::uint64_t now_milliseconds =
            static_cast<std::uint64_t>(now.tv_sec) * 1000U + static_cast<std::uint64_t>(now.tv_nsec) / 1000000U;
    const std::uint64_t boot_age_milliseconds = static_cast<std::uint64_t>(info.uptime) * 1000U;
    const std::uint64_t process_age_milliseconds = starttime_ticks * 1000U / static_cast<std::uint64_t>(clock_ticks);
    if (process_age_milliseconds > now_milliseconds) {
        return false;
    }
    start_time_milliseconds = now_milliseconds - boot_age_milliseconds + process_age_milliseconds;
    return true;
}

} // namespace

namespace detail {

bool parse_access_proc_stat(std::string_view text, AccessProcStatFields &fields) noexcept {
    // Layout per proc(5): (1) pid (2) comm (3) state ... (14) utime (15) stime (20) num_threads
    // (22) starttime. `comm` is parenthesized and may contain spaces or parentheses, so tokens
    // are counted after the final closing parenthesis. Skipped fields (e.g. tpgid) may hold
    // negative values, so only the sampled fields must parse as unsigned integers.
    const std::size_t comm_end = text.rfind(')');
    if (comm_end == std::string_view::npos) {
        return false;
    }
    std::size_t position = comm_end + 1;
    std::string_view token;
    for (std::size_t field = 3; field <= 22; ++field) {
        if (!next_token(text, position, token)) {
            return false;
        }
        std::uint64_t parsed = 0;
        switch (field) {
            case 14:
            case 15:
            case 20:
            case 22:
                if (!parse_uint_token(token, parsed)) {
                    return false;
                }
                break;
            default:
                break;
        }
        switch (field) {
            case 14:
                fields.utime_ticks = parsed;
                break;
            case 15:
                fields.stime_ticks = parsed;
                break;
            case 20:
                fields.threads = parsed;
                break;
            case 22:
                fields.starttime_ticks = parsed;
                break;
            default:
                break;
        }
    }
    return true;
}

bool parse_access_proc_status(std::string_view text, AccessProcStatusFields &fields) noexcept {
    bool resident_found = false;
    bool virtual_found = false;
    bool threads_found = false;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t line_end = text.find('\n', position);
        const std::string_view line = text.substr(position, line_end == std::string_view::npos ? std::string_view::npos
                                                                                               : line_end - position);
        position = line_end == std::string_view::npos ? text.size() : line_end + 1;
        std::size_t cursor = 0;
        std::string_view token;
        std::string_view key;
        if (next_token(line, cursor, key)) {
            std::uint64_t value = 0;
            if (key == "VmRSS:" && next_token(line, cursor, token) && parse_uint_token(token, value)) {
                fields.resident_kb = value;
                resident_found = true;
            } else if (key == "VmSize:" && next_token(line, cursor, token) && parse_uint_token(token, value)) {
                fields.virtual_kb = value;
                virtual_found = true;
            } else if (key == "Threads:" && next_token(line, cursor, token) && parse_uint_token(token, value)) {
                fields.threads = value;
                threads_found = true;
            }
        }
    }
    return resident_found && virtual_found && threads_found;
}

AccessProcessResourceStats collect_access_process_resources() noexcept {
    AccessProcessResourceStats stats;

    std::array<char, 8192> stat_storage{};
    std::string_view stat_text;
    AccessProcStatFields stat{};
    std::array<char, 8192> status_storage{};
    std::string_view status_text;
    AccessProcStatusFields status{};
    if (!read_small_file("/proc/self/stat", stat_storage, stat_text) || !parse_access_proc_stat(stat_text, stat) ||
        !read_small_file("/proc/self/status", status_storage, status_text) ||
        !parse_access_proc_status(status_text, status) || !count_open_fds(stats.open_fds) ||
        !resolve_max_fds(stats.max_fds)) {
        return stats;
    }

    const long clock_ticks = sysconf(_SC_CLK_TCK);
    const std::uint64_t cpu_ticks = stat.utime_ticks + stat.stime_ticks;
    if (clock_ticks <= 0 || cpu_ticks < stat.utime_ticks ||
        cpu_ticks > std::numeric_limits<std::uint64_t>::max() / 1000U) {
        return stats;
    }
    std::uint64_t start_time_milliseconds = 0;
    if (!resolve_start_time_milliseconds(stat.starttime_ticks, clock_ticks, start_time_milliseconds)) {
        return stats;
    }

    stats.cpu_milliseconds = cpu_ticks * 1000U / static_cast<std::uint64_t>(clock_ticks);
    stats.resident_bytes = status.resident_kb * 1024U;
    stats.virtual_bytes = status.virtual_kb * 1024U;
    stats.threads = stat.threads != 0 ? stat.threads : status.threads;
    stats.start_time_milliseconds = start_time_milliseconds;
    stats.available = true;
    return stats;
}

} // namespace detail

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
    value.process = detail::collect_access_process_resources();
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

    output.append("# HELP access_server_process_metrics_available Whether process resource metrics could be "
                  "sampled.\n");
    output.append("# TYPE access_server_process_metrics_available gauge\n");
    append_series(output, "access_server_process_metrics_available", {}, snapshot.process.available ? 1U : 0U);
    if (!snapshot.process.available) {
        return;
    }

    output.append("# HELP access_server_process_cpu_seconds_total User and system CPU time consumed by the "
                  "access-server process.\n");
    output.append("# TYPE access_server_process_cpu_seconds_total counter\n");
    append_seconds_series(output, "access_server_process_cpu_seconds_total", {}, snapshot.process.cpu_milliseconds);
    output.append("# HELP access_server_process_resident_memory_bytes Resident memory of the access-server "
                  "process.\n");
    output.append("# TYPE access_server_process_resident_memory_bytes gauge\n");
    append_series(output, "access_server_process_resident_memory_bytes", {}, snapshot.process.resident_bytes);
    output.append("# HELP access_server_process_virtual_memory_bytes Virtual memory of the access-server "
                  "process.\n");
    output.append("# TYPE access_server_process_virtual_memory_bytes gauge\n");
    append_series(output, "access_server_process_virtual_memory_bytes", {}, snapshot.process.virtual_bytes);
    output.append("# HELP access_server_process_threads Threads in the access-server process.\n");
    output.append("# TYPE access_server_process_threads gauge\n");
    append_series(output, "access_server_process_threads", {}, snapshot.process.threads);
    output.append("# HELP access_server_process_open_fds File descriptors currently open.\n");
    output.append("# TYPE access_server_process_open_fds gauge\n");
    append_series(output, "access_server_process_open_fds", {}, snapshot.process.open_fds);
    output.append("# HELP access_server_process_max_fds File descriptor limit in effect.\n");
    output.append("# TYPE access_server_process_max_fds gauge\n");
    append_series(output, "access_server_process_max_fds", {}, snapshot.process.max_fds);
    output.append("# HELP access_server_process_start_time_seconds Start time of the access-server process since "
                  "unix epoch.\n");
    output.append("# TYPE access_server_process_start_time_seconds gauge\n");
    append_seconds_series(output, "access_server_process_start_time_seconds", {},
                          snapshot.process.start_time_milliseconds);
}

} // namespace fiber::access_server
