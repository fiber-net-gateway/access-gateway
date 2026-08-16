#include <gtest/gtest.h>

#include "runtime/SubscriptionLifecycle.h"

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "NacosSnapshotTestBuilder.h"
#include "NacosSubscriptionStub.h"

namespace {

using namespace std::chrono_literals;
using fiber::access_server::SubscriptionLifecycle;
using fiber::access_server::SubscriptionLifecycleState;

class FakeConfigService final : public fiber::nacos::ConfigService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>;

    fiber::common::IoResult<void> start() noexcept override { return {}; }
    fiber::async::Task<void> shutdown() noexcept override { co_return; }

    fiber::async::Task<std::expected<std::shared_ptr<const fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>>
    get_config(std::string, std::string) noexcept override {
        co_return fiber::tests::make_config_data(fiber::nacos::ConfigState::NotFound);
    }

    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    publish(std::string, std::string, std::string, fiber::nacos::ConfigType,
            std::optional<std::string>) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }

    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    remove_config(std::string, std::string) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }

    std::expected<fiber::nacos::Subscription<fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group,
              fiber::nacos::Subscription<fiber::nacos::ConfigData>::NotifyCallback on_notify, void *context) override {
        EXPECT_EQ(data_id, "resource");
        EXPECT_EQ(group, "group");
        if (next_failure_) {
            fiber::nacos::ConfigServiceError error = std::move(*next_failure_);
            next_failure_.reset();
            return std::unexpected(std::move(error));
        }
        return subscriptions_.subscribe(on_notify, context);
    }

    void fail_next(fiber::nacos::ConfigServiceError error) { next_failure_ = std::move(error); }

    void prime(std::string content) {
        subscriptions_.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::Present, "md5", std::move(content)),
        });
    }

    void close() { subscriptions_.publish(Result{.kind = fiber::nacos::ResultKind::Closed}); }

private:
    fiber::tests::NacosSubscriptionStub<fiber::nacos::ConfigData> subscriptions_;
    std::optional<fiber::nacos::ConfigServiceError> next_failure_;
};

struct CallbackCapture {
    SubscriptionLifecycle *lifecycle = nullptr;
    SubscriptionLifecycleState state_during_replay = SubscriptionLifecycleState::Created;
    std::size_t values = 0;

    static void notify(void *context,
                       const fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData> &result) noexcept {
        auto &self = *static_cast<CallbackCapture *>(context);
        self.state_during_replay = self.lifecycle->state();
        if (result.kind == fiber::nacos::ResultKind::Closed) {
            self.lifecycle->fail(fiber::nacos::ConfigServiceError{
                    .code = fiber::nacos::ConfigServiceErrorCode::Shutdown,
                    .io_error = fiber::common::IoErr::NotConnected,
                    .message = "subscription closed",
            });
            return;
        }
        if (result.data) {
            ++self.values;
            (void) self.lifecycle->observe_value();
        }
    }
};

template<typename Function>
void run_on_loop(Function function) {
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        function(loop);
        loop.stop();
        co_return;
    });
    loop.run();
}

fiber::nacos::ConfigServiceError transient_error() {
    return fiber::nacos::ConfigServiceError{
            .code = fiber::nacos::ConfigServiceErrorCode::Transport,
            .io_error = fiber::common::IoErr::NotConnected,
            .message = "transient failure",
    };
}

TEST(SubscriptionLifecycleTest, KeepsConcreteControlPlaneStateBounded) {
    EXPECT_LE(sizeof(SubscriptionLifecycle), 112U);
    EXPECT_FALSE(std::is_polymorphic_v<SubscriptionLifecycle>);
}

TEST(SubscriptionLifecycleTest, HandlesSynchronousCachedReplayAndClosedGeneration) {
    run_on_loop([](fiber::event::EventLoop &loop) -> void {
        FakeConfigService service;
        service.prime("value");
        SubscriptionLifecycle lifecycle(loop);
        CallbackCapture capture{.lifecycle = &lifecycle};

        auto subscribed = lifecycle.subscribe(service, "resource", "group", &CallbackCapture::notify, &capture);
        EXPECT_TRUE(subscribed);
        if (!subscribed) {
            lifecycle.stop();
            return;
        }
        EXPECT_EQ(capture.state_during_replay, SubscriptionLifecycleState::Subscribing);
        EXPECT_EQ(capture.values, 1U);
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Subscribed);
        EXPECT_TRUE(lifecycle.subscribed());
        EXPECT_TRUE(lifecycle.first_value_received());
        EXPECT_EQ(lifecycle.generation(), 1U);

        const std::uint64_t before_close_revision = lifecycle.revision_version();
        service.close();
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Failed);
        EXPECT_FALSE(lifecycle.subscribed());
        EXPECT_FALSE(lifecycle.first_value_received());
        EXPECT_EQ(lifecycle.generation(), 2U);
        EXPECT_GT(lifecycle.revision_version(), before_close_revision);
        EXPECT_TRUE(lifecycle.last_failure());
        if (lifecycle.last_failure()) {
            EXPECT_EQ(lifecycle.last_failure()->io_error, fiber::common::IoErr::NotConnected);
        }

        lifecycle.stop();
        lifecycle.stop();
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Stopped);
    });
}

TEST(SubscriptionLifecycleTest, DoesNotInstallHandleAfterSynchronousClosedReplay) {
    run_on_loop([](fiber::event::EventLoop &loop) -> void {
        FakeConfigService service;
        service.close();
        SubscriptionLifecycle lifecycle(loop);
        CallbackCapture capture{.lifecycle = &lifecycle};

        auto subscribed = lifecycle.subscribe(service, "resource", "group", &CallbackCapture::notify, &capture);
        EXPECT_TRUE(subscribed);
        EXPECT_EQ(capture.state_during_replay, SubscriptionLifecycleState::Subscribing);
        EXPECT_EQ(capture.values, 0U);
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Failed);
        EXPECT_FALSE(lifecycle.subscribed());
        EXPECT_EQ(lifecycle.generation(), 1U);

        lifecycle.stop();
    });
}

TEST(SubscriptionLifecycleTest, OwnsTransientRetryStateAndCapsExponentialDelay) {
    run_on_loop([](fiber::event::EventLoop &loop) -> void {
        FakeConfigService service;
        SubscriptionLifecycle lifecycle(loop);
        const fiber::access_server::SubscriptionRetryPolicy policy{
                .initial_delay = 10ms,
                .maximum_delay = 25ms,
        };
        CallbackCapture capture{.lifecycle = &lifecycle};

        service.fail_next(transient_error());
        EXPECT_FALSE(lifecycle.subscribe(service, "resource", "group", &CallbackCapture::notify, &capture));
        auto first = lifecycle.schedule_retry(policy);
        EXPECT_TRUE(first);
        if (!first) {
            lifecycle.stop();
            return;
        }
        EXPECT_EQ(first->delay, 10ms);
        EXPECT_EQ(lifecycle.retry_attempt(), 1U);

        service.fail_next(transient_error());
        EXPECT_FALSE(lifecycle.subscribe(service, "resource", "group", &CallbackCapture::notify, &capture));
        auto second = lifecycle.schedule_retry(policy);
        EXPECT_TRUE(second);
        if (!second) {
            lifecycle.stop();
            return;
        }
        EXPECT_EQ(second->delay, 20ms);
        EXPECT_EQ(lifecycle.retry_attempt(), 2U);

        service.fail_next(transient_error());
        EXPECT_FALSE(lifecycle.subscribe(service, "resource", "group", &CallbackCapture::notify, &capture));
        auto third = lifecycle.schedule_retry(policy);
        EXPECT_TRUE(third);
        if (!third) {
            lifecycle.stop();
            return;
        }
        EXPECT_EQ(third->delay, 25ms);
        EXPECT_EQ(lifecycle.retry_attempt(), 3U);
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Retrying);

        const std::uint64_t pending_revision = third->revision_version;
        lifecycle.stop();
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Stopped);
        EXPECT_GT(lifecycle.revision_version(), pending_revision);
    });
}

TEST(SubscriptionLifecycleTest, KeepsPermanentFailureTerminalAndCanRollbackInitialStart) {
    run_on_loop([](fiber::event::EventLoop &loop) -> void {
        FakeConfigService service;
        SubscriptionLifecycle lifecycle(loop);
        CallbackCapture capture{.lifecycle = &lifecycle};
        service.fail_next(fiber::nacos::ConfigServiceError{
                .code = fiber::nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = fiber::common::IoErr::Invalid,
                .message = "permanent failure",
        });

        auto subscribed = lifecycle.subscribe(service, "resource", "group", &CallbackCapture::notify, &capture);
        EXPECT_FALSE(subscribed);
        if (subscribed) {
            lifecycle.stop();
            return;
        }
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Failed);
        EXPECT_FALSE(lifecycle.schedule_retry({}));

        lifecycle.reset_start_failure();
        EXPECT_EQ(lifecycle.state(), SubscriptionLifecycleState::Created);
        EXPECT_FALSE(lifecycle.last_failure());
    });
}

} // namespace
