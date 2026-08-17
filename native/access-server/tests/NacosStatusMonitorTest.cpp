#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/async/Watch.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>

#include "observability/AccessDiscoveryMetrics.h"
#include "runtime/NacosStatusMonitor.h"

namespace fiber::access_server {
namespace {

class FakeConfigService final : public nacos::ConfigService {
public:
    FakeConfigService() {
        publisher_ = status_.acquire_publisher();
        FIBER_ASSERT(publisher_);
    }

    common::IoResult<void> start() noexcept override { return {}; }

    async::Task<void> shutdown() noexcept override {
        publish_status(nacos::ConfigServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Stopping,
                                .failure = nacos::NacosServiceFailureCategory::Shutdown,
                        },
        });
        publish_status(nacos::ConfigServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Stopped,
                                .failure = nacos::NacosServiceFailureCategory::Shutdown,
                        },
        });
        co_return;
    }

    StatusSubscriber subscribe_status() override { return status_.subscribe(); }

    async::Task<std::expected<std::shared_ptr<const nacos::ConfigData>, nacos::ConfigServiceError>>
    get_config(std::string, std::string) noexcept override {
        co_return std::unexpected(nacos::ConfigServiceError{.code = nacos::ConfigServiceErrorCode::Shutdown});
    }

    async::Task<std::expected<void, nacos::ConfigServiceError>>
    publish(std::string, std::string, std::string, nacos::ConfigType, std::optional<std::string>) noexcept override {
        co_return std::unexpected(nacos::ConfigServiceError{.code = nacos::ConfigServiceErrorCode::Shutdown});
    }

    async::Task<std::expected<void, nacos::ConfigServiceError>> remove_config(std::string,
                                                                              std::string) noexcept override {
        co_return std::unexpected(nacos::ConfigServiceError{.code = nacos::ConfigServiceErrorCode::Shutdown});
    }

    std::expected<nacos::Subscription<nacos::ConfigData>, nacos::ConfigServiceError>
    subscribe(std::string_view, std::string_view, nacos::Subscription<nacos::ConfigData>::NotifyCallback,
              void *) override {
        return std::unexpected(nacos::ConfigServiceError{.code = nacos::ConfigServiceErrorCode::Shutdown});
    }

    void publish_status(nacos::ConfigServiceStatus status) { publisher_->publish(std::move(status)); }

private:
    async::Watch<nacos::ConfigServiceStatus> status_{nacos::ConfigServiceStatus{}};
    std::optional<async::Watch<nacos::ConfigServiceStatus>::Publisher> publisher_;
};

class FakeNamingService final : public nacos::NamingService {
public:
    FakeNamingService() {
        publisher_ = status_.acquire_publisher();
        FIBER_ASSERT(publisher_);
    }

    common::IoResult<void> start() noexcept override { return {}; }

    async::Task<void> shutdown() noexcept override {
        publish_status(nacos::NamingServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Stopping,
                                .failure = nacos::NacosServiceFailureCategory::Shutdown,
                        },
        });
        publish_status(nacos::NamingServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Stopped,
                                .failure = nacos::NacosServiceFailureCategory::Shutdown,
                        },
        });
        co_return;
    }

    StatusSubscriber subscribe_status() override { return status_.subscribe(); }

    async::Task<std::expected<std::shared_ptr<const nacos::ServiceInfo>, nacos::NamingServiceError>>
    get(std::string, std::string) noexcept override {
        co_return std::unexpected(nacos::NamingServiceError{.code = nacos::NamingServiceErrorCode::Shutdown});
    }

    std::expected<nacos::Subscription<nacos::ServiceInfo>, nacos::NamingServiceError>
    subscribe(std::string_view, std::string_view, nacos::Subscription<nacos::ServiceInfo>::NotifyCallback,
              void *) override {
        return std::unexpected(nacos::NamingServiceError{.code = nacos::NamingServiceErrorCode::Shutdown});
    }

    std::expected<nacos::InstanceRegistration, nacos::NamingServiceError> registry(std::string_view, std::string_view,
                                                                                   nacos::Instance) override {
        return std::unexpected(nacos::NamingServiceError{.code = nacos::NamingServiceErrorCode::Shutdown});
    }

    void publish_status(nacos::NamingServiceStatus status) { publisher_->publish(std::move(status)); }

private:
    async::Watch<nacos::NamingServiceStatus> status_{nacos::NamingServiceStatus{}};
    std::optional<async::Watch<nacos::NamingServiceStatus>::Publisher> publisher_;
};

TEST(NacosStatusMonitorTest, MapsLatestStatusAndJoinsSubscribersBeforeShutdown) {
    event::EventLoop loop;
    AccessDiscoveryMetrics metrics(loop);
    FakeConfigService config;
    FakeNamingService naming;
    NacosStatusMonitor monitor(loop, config, naming, metrics.observer());
    bool completed = false;

    config.publish_status(nacos::ConfigServiceStatus{
            .connection =
                    {
                            .phase = nacos::NacosServicePhase::Connecting,
                            .failure = nacos::NacosServiceFailureCategory::AuthenticationUnavailable,
                            .reconnect_attempt_count = 1,
                    },
            .subscriptions =
                    {
                            .active_count = 2,
                            .pending_count = 2,
                    },
    });

    async::spawn(loop, [&]() -> async::DetachedTask {
        metrics.observer().set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Running);
        monitor.start();

        AccessDiscoveryStatus current = metrics.status();
        const std::size_t config_index = static_cast<std::size_t>(AccessNacosTransportComponent::ConfigService);
        const std::size_t naming_index = static_cast<std::size_t>(AccessNacosTransportComponent::NamingService);
        EXPECT_EQ(current.transport[config_index].phase, AccessNacosTransportPhase::Connecting);
        EXPECT_EQ(current.transport[config_index].failure, AccessNacosTransportFailure::AuthenticationUnavailable);
        EXPECT_FALSE(current.transport[config_index].rpc_available);
        EXPECT_EQ(current.lifecycle[static_cast<std::size_t>(AccessNacosComponent::ConfigService)],
                  AccessNacosLifecycleState::Running);

        config.publish_status(nacos::ConfigServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::ReconnectBackoff,
                                .failure = nacos::NacosServiceFailureCategory::Transport,
                                .connection_ready_count = 1,
                                .disconnect_count = 1,
                                .reconnect_attempt_count = 2,
                        },
                .subscriptions =
                        {
                                .active_count = 3,
                                .pending_count = 2,
                                .registered_count = 1,
                                .synchronized_count = 1,
                        },
        });
        naming.publish_status(nacos::NamingServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Ready,
                                .rpc_available = true,
                                .connection_ready_count = 1,
                        },
                .subscriptions =
                        {
                                .active_count = 4,
                                .registered_count = 4,
                                .synchronized_count = 4,
                        },
                .registrations =
                        {
                                .active_count = 1,
                                .registered_count = 1,
                        },
        });
        co_await async::yield();
        co_await async::yield();

        current = metrics.status();
        EXPECT_EQ(current.transport[config_index].phase, AccessNacosTransportPhase::ReconnectBackoff);
        EXPECT_EQ(current.transport[config_index].failure, AccessNacosTransportFailure::Transport);
        EXPECT_FALSE(current.transport[config_index].rpc_available);
        EXPECT_EQ(current.transport[config_index].subscriptions.pending, 2u);
        EXPECT_EQ(current.transport[naming_index].phase, AccessNacosTransportPhase::Ready);
        EXPECT_TRUE(current.transport[naming_index].rpc_available);
        EXPECT_EQ(current.transport[naming_index].registrations.registered, 1u);

        config.publish_status(nacos::ConfigServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Ready,
                                .rpc_available = true,
                                .connection_ready_count = 2,
                                .disconnect_count = 1,
                                .reconnect_attempt_count = 2,
                        },
                .subscriptions =
                        {
                                .active_count = 3,
                                .registered_count = 3,
                                .synchronized_count = 3,
                        },
        });
        co_await async::yield();
        current = metrics.status();
        EXPECT_EQ(current.transport[config_index].phase, AccessNacosTransportPhase::Ready);
        EXPECT_EQ(current.transport[config_index].failure, AccessNacosTransportFailure::None);
        EXPECT_TRUE(current.transport[config_index].rpc_available);

        co_await naming.shutdown();
        co_await config.shutdown();
        co_await monitor.shutdown();
        current = metrics.status();
        EXPECT_EQ(current.transport[config_index].phase, AccessNacosTransportPhase::Stopped);
        EXPECT_EQ(current.transport[naming_index].phase, AccessNacosTransportPhase::Stopped);
        EXPECT_FALSE(current.transport[config_index].rpc_available);
        EXPECT_FALSE(current.transport[naming_index].rpc_available);

        co_await monitor.shutdown();
        config.publish_status(nacos::ConfigServiceStatus{
                .connection =
                        {
                                .phase = nacos::NacosServicePhase::Ready,
                                .rpc_available = true,
                        },
        });
        co_await async::yield();
        EXPECT_EQ(metrics.status().transport[config_index].phase, AccessNacosTransportPhase::Stopped);

        completed = true;
        loop.stop();
    });
    loop.run();

    EXPECT_TRUE(completed);
}

} // namespace
} // namespace fiber::access_server
