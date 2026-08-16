#ifndef FIBER_ACCESS_SERVER_ACCESS_RUNTIME_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_RUNTIME_METRICS_H

#include "AccessConfigMetrics.h"
#include "AccessDiscoveryMetrics.h"

#include <chrono>
#include <string>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

// Stable aggregation boundary between application-owned metric domains and the
// Fiber Prometheus collector used by the HTTP request workers.
class AccessRuntimeMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessRuntimeMetrics(event::EventLoop &nacos_owner) noexcept;

    [[nodiscard]] AccessConfigMetrics &config() noexcept { return config_; }
    [[nodiscard]] AccessDiscoveryMetrics &discovery() noexcept { return discovery_; }

    void append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const;

private:
    AccessConfigMetrics config_;
    AccessDiscoveryMetrics discovery_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_RUNTIME_METRICS_H
