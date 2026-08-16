#ifndef FIBER_ACCESS_SERVER_ACCESS_RUNTIME_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_RUNTIME_METRICS_H

#include "AccessConfigMetrics.h"
#include "AccessDiscoveryMetrics.h"
#include "AccessProcessMetrics.h"
#include "AccessTlsMetrics.h"

#include <chrono>
#include <string>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

// Stable aggregation boundary between application-owned metric domains and the
// Fiber Prometheus collector used by the HTTP request workers.
class AccessRuntimeMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessRuntimeMetrics(event::EventLoop &nacos_owner,
                                  AccessProcessMetricsSources process_sources = {}) noexcept;

    [[nodiscard]] AccessConfigMetrics &config() noexcept { return config_; }
    [[nodiscard]] AccessDiscoveryMetrics &discovery() noexcept { return discovery_; }
    [[nodiscard]] const AccessDiscoveryMetrics &discovery() const noexcept { return discovery_; }
    [[nodiscard]] AccessTlsMetrics &tls() noexcept { return tls_; }

    void append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const;

private:
    AccessConfigMetrics config_;
    AccessDiscoveryMetrics discovery_;
    AccessTlsMetrics tls_;
    AccessProcessMetrics process_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_RUNTIME_METRICS_H
