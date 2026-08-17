#ifndef FIBER_ACCESS_SERVER_NACOS_STATUS_MONITOR_H
#define FIBER_ACCESS_SERVER_NACOS_STATUS_MONITOR_H

#include "../observability/AccessDiscoveryMetrics.h"

#include <cstdint>
#include <optional>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {

// Consumes Fiber's bounded latest-value service watches on their owner loop.
// Subscriber tasks are explicitly stopped and joined before the services are
// destroyed; no identifier, address, credential, or diagnostic text crosses
// this boundary.
class NacosStatusMonitor final : public common::NonCopyable, public common::NonMovable {
public:
    NacosStatusMonitor(event::EventLoop &owner, nacos::ConfigService &config_service,
                       nacos::NamingService &naming_service, AccessDiscoveryMetricsObserver metrics) noexcept;
    ~NacosStatusMonitor() noexcept;

    void start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;

private:
    enum class State : std::uint8_t {
        Created,
        Running,
        Stopped,
    };

    [[nodiscard]] async::DetachedTask watch_config(nacos::ConfigService::StatusSubscriber subscriber,
                                                   std::uint64_t version) noexcept;
    [[nodiscard]] async::DetachedTask watch_naming(nacos::NamingService::StatusSubscriber subscriber,
                                                   std::uint64_t version) noexcept;

    event::EventLoop *owner_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    nacos::NamingService *naming_service_ = nullptr;
    AccessDiscoveryMetricsObserver metrics_;
    async::Watch<bool> stopping_{false};
    std::optional<async::Watch<bool>::Publisher> stopping_publisher_;
    async::WaitGroup tasks_;
    State state_ = State::Created;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_NACOS_STATUS_MONITOR_H
