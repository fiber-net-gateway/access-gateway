#ifndef FIBER_ACCESS_SERVER_ACCESS_PROCESS_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_PROCESS_METRICS_H

#include <string>

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

// Identifier-free copy of the Fiber process facilities used by the renderer
// and deterministic tests. Fiber owns all source counters and their lifetime.
struct AccessProcessMetricsSnapshot {
    bool logging_available = false;
    log::LogQueueStats log_queue;
    log::AppenderStats log_appender;
    bool cat_enabled = false;
    cat::CatClientState cat_state = cat::CatClientState::Created;
    cat::CatClientStats cat;
};

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
