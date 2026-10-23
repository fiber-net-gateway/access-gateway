#ifndef FIBER_ACCESS_SERVER_TEMP_CLUSTER_CONFIG_WATCHER_H
#define FIBER_ACCESS_SERVER_TEMP_CLUSTER_CONFIG_WATCHER_H

#include "SubscriptionLifecycle.h"
#include "TempClusterStore.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

enum class TempClusterConfigWatcherState : std::uint8_t {
    Created,
    Running,
    Failed,
    Stopping,
    Stopped,
};

struct TempClusterConfigWatcherOptions {
    std::string data_id = std::string(kTempClusterConfigDataId);
    std::string group = std::string(kDefaultNacosGroup);
};

struct TempClusterConfigWatcherFailure {
    std::string stage;
    std::string code;
    std::string md5;
    std::string message;
};

// Subscribes the test-environment temp-cluster whitelist. Unlike the
// gray/config/TLS watchers this Data ID is an ops-managed test document, not a
// console-published resource, so it contributes no activation evidence.
class TempClusterConfigWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    TempClusterConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service, TempClusterStore &store,
                             TempClusterConfigWatcherOptions options = {}) noexcept;
    ~TempClusterConfigWatcher() noexcept;

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_readiness() { return readiness_.subscribe(); }

    [[nodiscard]] TempClusterConfigWatcherState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<TempClusterConfigWatcherFailure> &last_failure() const noexcept {
        return last_failure_;
    }

private:
    static void on_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    void apply(const nacos::ConfigData &data);

    async::Watch<bool> readiness_{false};
    std::optional<async::Watch<bool>::Publisher> readiness_publisher_;
    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    TempClusterStore *store_ = nullptr;
    TempClusterConfigWatcherOptions options_;
    SubscriptionLifecycle subscription_;
    std::optional<TempClusterConfigWatcherFailure> last_failure_;
    TempClusterConfigWatcherState state_ = TempClusterConfigWatcherState::Created;
    std::string observed_md5_;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TEMP_CLUSTER_CONFIG_WATCHER_H
