#ifndef FIBER_ACCESS_SERVER_ACCESS_PROCESS_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_PROCESS_METRICS_H

#include <cstdint>
#include <string>
#include <string_view>

#include <fiber/cat/CatClient.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/log/LoggerManager.h>

namespace fiber::access_server {

struct AccessProcessMetricsSources {
    // Borrowed sources must outlive AccessRuntimeMetrics and all in-flight
    // scrapes. AccessServerRuntime enforces that shutdown order.
    const log::LoggerManager *logger = nullptr;
    log::AppenderId log_appender = log::kInvalidAppenderId;
    const cat::CatClient *cat_client = nullptr;
};

// Process-local resource usage sampled from /proc at scrape time. All-or-
// nothing: `available` is false and every field stays zero when any source
// cannot be read.
struct AccessProcessResourceStats {
    bool available = false;
    std::uint64_t cpu_milliseconds = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t virtual_bytes = 0;
    std::uint64_t threads = 0;
    std::uint64_t open_fds = 0;
    std::uint64_t max_fds = 0;
    std::uint64_t start_time_milliseconds = 0;
};

// Identifier-free copy of the Fiber process facilities used by the renderer
// and deterministic tests. Fiber owns all source counters and their lifetime.
struct AccessProcessMetricsSnapshot {
    bool logging_available = false;
    log::LogQueueStats log_queue;
    log::AppenderStats log_appender;
    bool cat_enabled = false;
    cat::CatClientState cat_state = cat::CatClientState::Created;
    cat::CatClientStats cat;
    AccessProcessResourceStats process;
};

// /proc parsing internals, exposed for deterministic tests only.
namespace detail {

struct AccessProcStatFields {
    std::uint64_t utime_ticks = 0;
    std::uint64_t stime_ticks = 0;
    std::uint64_t threads = 0;
    std::uint64_t starttime_ticks = 0;
};

struct AccessProcStatusFields {
    std::uint64_t resident_kb = 0;
    std::uint64_t virtual_kb = 0;
    std::uint64_t threads = 0;
};

[[nodiscard]] bool parse_access_proc_stat(std::string_view text, AccessProcStatFields &fields) noexcept;

[[nodiscard]] bool parse_access_proc_status(std::string_view text, AccessProcStatusFields &fields) noexcept;

[[nodiscard]] AccessProcessResourceStats collect_access_process_resources() noexcept;

} // namespace detail

void append_access_process_metrics(std::string &output, const AccessProcessMetricsSnapshot &snapshot);

class AccessProcessMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessProcessMetrics(AccessProcessMetricsSources sources = {}) noexcept : sources_(sources) {}

    [[nodiscard]] AccessProcessMetricsSnapshot snapshot() const noexcept;
    void append_prometheus(std::string &output) const;

private:
    AccessProcessMetricsSources sources_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_PROCESS_METRICS_H
