#include "runtime/AccessInstanceRegistration.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

struct RegistrationState {
    async::Watch<nacos::RegistrationStatus> status{nacos::RegistrationStatus{}};
    std::optional<async::Watch<nacos::RegistrationStatus>::Publisher> publisher = status.acquire_publisher();
    bool closed = false;
};

class RegistrationNamingService final : public nacos::NamingService {
public:
    common::IoResult<void> start() noexcept override { return {}; }
    async::Task<void> shutdown() noexcept override { co_return; }
    StatusSubscriber subscribe_status() override { return service_status_.subscribe(); }

    async::Task<std::expected<std::shared_ptr<const nacos::ServiceInfo>, nacos::NamingServiceError>>
    get(std::string, std::string) noexcept override {
        co_return std::unexpected(nacos::NamingServiceError{.code = nacos::NamingServiceErrorCode::Server});
    }

    std::expected<nacos::Subscription<nacos::ServiceInfo>, nacos::NamingServiceError>
    subscribe(std::string_view, std::string_view, nacos::Subscription<nacos::ServiceInfo>::NotifyCallback,
              void *) override {
        return std::unexpected(nacos::NamingServiceError{.code = nacos::NamingServiceErrorCode::Server});
    }

    std::expected<nacos::InstanceRegistration, nacos::NamingServiceError>
    registry(std::string_view service_name, std::string_view group, nacos::Instance instance) override {
        observed_service = service_name;
        observed_group = group;
        observed_instance = std::move(instance);
        state = std::make_shared<RegistrationState>();
        state->publisher->publish(next_status);
        return nacos::InstanceRegistration(state, state.get(), &update, &subscribe_registration, &close_registration);
    }

    static std::expected<void, nacos::NamingServiceError> update(void *, nacos::Instance) noexcept { return {}; }

    static nacos::InstanceRegistration::StatusSubscriber subscribe_registration(void *context) {
        return static_cast<RegistrationState *>(context)->status.subscribe();
    }

    static void close_registration(void *context) noexcept {
        auto &registration = *static_cast<RegistrationState *>(context);
        registration.closed = true;
        registration.publisher->publish(nacos::RegistrationStatus{.state = nacos::RegistrationState::Closed});
    }

    nacos::RegistrationStatus next_status{.state = nacos::RegistrationState::Registered};
    std::string observed_service;
    std::string observed_group;
    nacos::Instance observed_instance;
    std::shared_ptr<RegistrationState> state;

private:
    async::Watch<nacos::NamingServiceStatus> service_status_{nacos::NamingServiceStatus{}};
};

TEST(AccessInstanceRegistrationTest, WaitsForRegisteredAndClosesOnOwnerLoop) {
    event::EventLoop loop;
    RegistrationNamingService naming;
    AccessInstanceRegistration registration(
            loop, naming,
            AccessInstanceRegistrationOptions{
                    .service_name = "unified-access-server",
                    .group = "ACCESS-SERVER",
                    .cluster_name = "prod-blue",
                    .advertise_address = net::IpAddress::loopback_v4(),
                    .timeout = std::chrono::milliseconds(100),
                    .metadata = {nacos::NamingMetadataEntry{.key = "g", .value = "projects"}},
            });
    bool completed = false;
    async::spawn(loop, [&]() -> async::DetachedTask {
        auto started = co_await registration.start(net::IpAddress::any_v4(), 18000);
        EXPECT_TRUE(started);
        EXPECT_EQ(naming.observed_service, "unified-access-server");
        EXPECT_EQ(naming.observed_group, "ACCESS-SERVER");
        EXPECT_EQ(naming.observed_instance.ip, "127.0.0.1");
        EXPECT_EQ(naming.observed_instance.port, 18000);
        EXPECT_EQ(naming.observed_instance.cluster_name, "prod-blue");
        EXPECT_EQ(naming.observed_instance.metadata.size(), 1U);
        if (!naming.observed_instance.metadata.empty()) {
            EXPECT_EQ(naming.observed_instance.metadata[0].key, "g");
        }
        registration.close();
        EXPECT_TRUE(naming.state);
        if (naming.state) {
            EXPECT_TRUE(naming.state->closed);
        }
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(AccessInstanceRegistrationTest, FailsStartupWhenRegistrationIsRejected) {
    event::EventLoop loop;
    RegistrationNamingService naming;
    naming.next_status = nacos::RegistrationStatus{
            .state = nacos::RegistrationState::Failed,
            .error = std::make_shared<nacos::NamingServiceError>(nacos::NamingServiceError{
                    .code = nacos::NamingServiceErrorCode::Transport,
                    .io_error = common::IoErr::NotConnected,
            }),
    };
    AccessInstanceRegistration registration(loop, naming, AccessInstanceRegistrationOptions{});
    bool completed = false;
    async::spawn(loop, [&]() -> async::DetachedTask {
        auto started = co_await registration.start(net::IpAddress::loopback_v4(), 8000);
        EXPECT_FALSE(started);
        if (started) {
            registration.close();
            completed = true;
            loop.stop();
            co_return;
        }
        EXPECT_EQ(started.error().code, AccessServerRuntimeErrorCode::WaitNacosRegistration);
        EXPECT_EQ(started.error().io_error, common::IoErr::NotConnected);
        EXPECT_TRUE(naming.state);
        if (naming.state) {
            EXPECT_TRUE(naming.state->closed);
        }
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
} // namespace fiber::access_server
