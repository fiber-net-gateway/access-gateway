#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "NacosSnapshotTestBuilder.h"
#include "NacosSubscriptionStub.h"
#include "runtime/AccessConfigWatcher.h"

namespace {

using namespace std::chrono_literals;

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
              fiber::nacos::Subscription<fiber::nacos::ConfigData>::NotifyCallback on_notify, void *ctx) override {
        const std::string key = make_key(data_id, group);
        ++subscribe_attempts_[key];
        const auto failure = failures_.find(key);
        if (failure != failures_.end() && failure->second.remaining != 0) {
            --failure->second.remaining;
            return std::unexpected(failure->second.error);
        }
        auto [iterator, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return iterator->second->subscriptions.subscribe(on_notify, ctx);
    }

    void fail_subscriptions(std::string_view data_id, std::size_t count, fiber::nacos::ConfigServiceError error) {
        failures_[make_key(data_id, fiber::access_server::kProjectRouteGroup)] = FailurePlan{
                .remaining = count,
                .error = std::move(error),
        };
    }

    void prime(std::string_view data_id, std::string content, std::string md5 = {}) {
        auto [iterator, inserted] = entries_.try_emplace(make_key(data_id, fiber::access_server::kProjectRouteGroup),
                                                         std::make_unique<Entry>());
        (void) inserted;
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::Present, std::move(md5),
                                                       std::move(content)),
        });
    }

    void push(std::string_view data_id, std::string content, std::string md5 = {}) {
        const auto iterator = entries_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::Present, std::move(md5),
                                                       std::move(content)),
        });
    }

    void push_not_found(std::string_view data_id) {
        const auto iterator = entries_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::NotFound),
        });
    }

    void close(std::string_view data_id) {
        const auto iterator = entries_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Closed,
        });
    }

    [[nodiscard]] std::size_t subscriptions(std::string_view data_id) const {
        const auto iterator = entries_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        return iterator == entries_.end() ? 0 : iterator->second->subscriptions.subscription_count();
    }

    [[nodiscard]] std::size_t subscribe_attempts(std::string_view data_id) const {
        const auto iterator = subscribe_attempts_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        return iterator == subscribe_attempts_.end() ? 0 : iterator->second;
    }

private:
    struct Entry {
        fiber::tests::NacosSubscriptionStub<fiber::nacos::ConfigData> subscriptions;
    };

    struct FailurePlan {
        std::size_t remaining = 0;
        fiber::nacos::ConfigServiceError error;
    };

    static std::string make_key(std::string_view data_id, std::string_view group) {
        std::string key(data_id);
        key.push_back('\n');
        key.append(group);
        return key;
    }

    std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
    std::map<std::string, FailurePlan, std::less<>> failures_;
    std::map<std::string, std::size_t, std::less<>> subscribe_attempts_;
};

fiber::async::Task<void> yield_updates() {
    for (std::size_t i = 0; i < 8; ++i) {
        co_await fiber::async::yield();
    }
}

std::string route_config(std::int32_t version, std::string_view host, std::string_view service) {
    return std::string("{\"version\":") + std::to_string(version) + ",\"host\":{\"" + std::string(host) +
           "\":{}},\"routes\":[{\"path\":\"/\",\"service\":\"" + std::string(service) + "\"}]}";
}

TEST(AccessConfigWatcherTest, ReconcilesProjectsAndRetainsLastValidSnapshots) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    std::size_t observer_updates = 0;
    fiber::access_server::RouteSnapshotObserver observer{
            .context = &observer_updates,
            .on_update =
                    [](void *context, std::shared_ptr<const fiber::access_server::AccessRouteSnapshot>) noexcept {
                        ++*static_cast<std::size_t *>(context);
                    },
    };
    fiber::access_server::AccessConfigWatcher watcher(loop, service, store, {}, observer);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        auto readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state,
                  fiber::access_server::AccessConfigReadinessState::WaitingForProjectList);
        EXPECT_TRUE(watcher.start());
        EXPECT_EQ(service.subscriptions(fiber::access_server::kProjectListDataId), 1u);

        service.push(fiber::access_server::kProjectListDataId, "a;b");
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state,
                  fiber::access_server::AccessConfigReadinessState::SynchronizingProjects);
        EXPECT_EQ(readiness_snapshot.value->desired_projects, 2u);
        EXPECT_EQ(readiness_snapshot.value->subscribed_projects, 2u);
        EXPECT_EQ(readiness_snapshot.value->synchronized_projects, 0u);
        EXPECT_TRUE(watcher.initial_project_list_received());
        EXPECT_EQ(watcher.project_subscription_count(), 2u);
        EXPECT_EQ(watcher.active_project_subscription_count(), 2u);
        EXPECT_EQ(service.subscriptions("ploto.unified-access.route.a"), 1u);
        EXPECT_EQ(service.subscriptions("ploto.unified-access.route.b"), 1u);

        service.push("ploto.unified-access.route.a", route_config(1, "a.example.com", "orders"), "a1");
        service.push("ploto.unified-access.route.b", route_config(1, "b.example.com", "billing"), "b1");
        co_await yield_updates();
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
        EXPECT_EQ(readiness_snapshot.value->synchronized_projects, 2u);
        EXPECT_EQ(readiness_snapshot.value->rejected_projects, 0u);
        EXPECT_TRUE(store.pin()->match_host("a.example.com"));
        EXPECT_TRUE(store.pin()->match_host("b.example.com"));
        const auto valid = store.pin();

        service.push(fiber::access_server::kProjectListDataId,
                     std::string(fiber::access_server::kAccessConfigLimits.project_list.max_payload_bytes + 1U, 'x'),
                     "oversized-list");
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 2u);
        EXPECT_EQ(store.pin(), valid);
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        if (readiness_snapshot.value) {
            EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Unavailable);
        }
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->stage, fiber::access_server::AccessConfigWatcherFailureStage::Decode);
            EXPECT_EQ(watcher.last_failure()->error.code, fiber::access_server::AccessConfigErrorCode::LimitExceeded);
        }

        service.push(fiber::access_server::kProjectListDataId, "a;b", "valid-list");
        co_await yield_updates();
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        if (readiness_snapshot.value) {
            EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
        }

        service.push("ploto.unified-access.route.a", route_config(1, "changed.example.com", "orders"), "same");
        service.push("ploto.unified-access.route.b", "{", "invalid");
        co_await yield_updates();
        EXPECT_EQ(store.pin(), valid);
        EXPECT_FALSE(store.pin()->match_host("changed.example.com"));
        EXPECT_EQ(watcher.failed_updates(), 2u);
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->data_id, "ploto.unified-access.route.b");
            EXPECT_EQ(watcher.last_failure()->stage, fiber::access_server::AccessConfigWatcherFailureStage::Decode);
        }
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
        EXPECT_EQ(readiness_snapshot.value->rejected_projects, 1u);

        service.push("ploto.unified-access.route.a", "", "empty");
        co_await yield_updates();
        EXPECT_EQ(store.pin(), valid);

        service.push(fiber::access_server::kProjectListDataId, "a;b;c");
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 3u);
        EXPECT_EQ(service.subscriptions("ploto.unified-access.route.c"), 1u);
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state,
                  fiber::access_server::AccessConfigReadinessState::SynchronizingProjects);
        service.push_not_found("ploto.unified-access.route.c");
        co_await yield_updates();
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);

        service.push(fiber::access_server::kProjectListDataId, "b;c");
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 2u);
        EXPECT_FALSE(store.pin()->match_host("a.example.com"));
        EXPECT_TRUE(store.pin()->match_host("b.example.com"));

        service.push_not_found(fiber::access_server::kProjectListDataId);
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 0u);
        EXPECT_TRUE(store.pin()->projects().empty());
        EXPECT_GE(observer_updates, 4u);
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
        EXPECT_EQ(readiness_snapshot.value->desired_projects, 0u);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(watcher.state(), fiber::access_server::AccessConfigWatcherState::Stopped);
}

TEST(AccessConfigWatcherTest, RetriesTransientProjectSubscriptionsAndReportsTypedReadiness) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    service.fail_subscriptions("ploto.unified-access.route.b", 2,
                               fiber::nacos::ConfigServiceError{
                                       .code = fiber::nacos::ConfigServiceErrorCode::Transport,
                                       .io_error = fiber::common::IoErr::NotConnected,
                                       .message = "transient subscription failure",
                               });
    service.prime("ploto.unified-access.route.b", "{", "bad-md5");
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::AccessConfigWatcherOptions options;
    options.subscription_retry_initial_delay = 0ms;
    options.subscription_retry_max_delay = 0ms;
    fiber::access_server::AccessConfigWatcher watcher(loop, service, store, options);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "b");

        auto snapshot = readiness.current();
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::SynchronizingProjects);
            EXPECT_EQ(snapshot.value->desired_projects, 1u);
            EXPECT_EQ(snapshot.value->subscribed_projects, 0u);
            EXPECT_EQ(snapshot.value->retrying_projects, 1u);
        }
        EXPECT_EQ(watcher.project_subscription_count(), 1u);
        EXPECT_EQ(watcher.active_project_subscription_count(), 0u);
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->stage,
                      fiber::access_server::AccessConfigWatcherFailureStage::Subscription);
            EXPECT_EQ(watcher.last_failure()->data_id, "ploto.unified-access.route.b");
            EXPECT_EQ(watcher.last_failure()->io_error, fiber::common::IoErr::NotConnected);
        }

        co_await yield_updates();
        EXPECT_EQ(service.subscribe_attempts("ploto.unified-access.route.b"), 3u);
        EXPECT_EQ(watcher.active_project_subscription_count(), 1u);
        snapshot = readiness.current();
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
            EXPECT_EQ(snapshot.value->synchronized_projects, 1u);
            EXPECT_EQ(snapshot.value->rejected_projects, 1u);
        }
        auto status = watcher.project_status("b");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->subscription_state, fiber::access_server::AccessProjectSubscriptionState::Subscribed);
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Rejected);
            EXPECT_TRUE(status->first_value_received);
            EXPECT_TRUE(status->synchronized);
            EXPECT_EQ(status->observed_md5, "bad-md5");
        }

        service.push("ploto.unified-access.route.b", route_config(1, "b.example.com", "billing"), "good-md5");
        co_await yield_updates();
        EXPECT_TRUE(store.pin()->match_host("b.example.com"));
        status = watcher.project_status("b");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Accepted);
            EXPECT_EQ(status->observed_version, 1);
            EXPECT_GT(status->published_generation, 0u);
        }
        snapshot = readiness.current();
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
            EXPECT_EQ(snapshot.value->rejected_projects, 0u);
        }

        service.fail_subscriptions("ploto.unified-access.route.bad", 1,
                                   fiber::nacos::ConfigServiceError{
                                           .code = fiber::nacos::ConfigServiceErrorCode::InvalidArgument,
                                           .io_error = fiber::common::IoErr::Invalid,
                                           .message = "permanent subscription failure",
                                   });
        service.push(fiber::access_server::kProjectListDataId, "b;bad");
        status = watcher.project_status("bad");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->subscription_state, fiber::access_server::AccessProjectSubscriptionState::Failed);
        }
        EXPECT_EQ(service.subscribe_attempts("ploto.unified-access.route.bad"), 1u);
        snapshot = readiness.current();
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::SynchronizingProjects);
        }
        service.push(fiber::access_server::kProjectListDataId, "b");
        snapshot = readiness.current();
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
        }

        service.close(fiber::access_server::kProjectListDataId);
        snapshot = readiness.current();
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Unavailable);
            EXPECT_EQ(snapshot.value->io_error, fiber::common::IoErr::NotConnected);
        }

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(AccessConfigWatcherTest, ShutdownWinsAgainstQueuedConfigCallbacks) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    service.fail_subscriptions("ploto.unified-access.route.c", 100,
                               fiber::nacos::ConfigServiceError{
                                       .code = fiber::nacos::ConfigServiceErrorCode::Transport,
                                       .io_error = fiber::common::IoErr::NotConnected,
                                       .message = "retry remains pending",
                               });
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::AccessConfigWatcherOptions options;
    options.subscription_retry_initial_delay = 1h;
    options.subscription_retry_max_delay = 1h;
    fiber::access_server::AccessConfigWatcher watcher(loop, service, store, options);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "a;b");
        co_await yield_updates();
        service.push("ploto.unified-access.route.a", route_config(1, "a.example.com", "orders"));
        service.push(fiber::access_server::kProjectListDataId, "b;c");
        auto status = watcher.project_status("c");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->subscription_state, fiber::access_server::AccessProjectSubscriptionState::Retrying);
        }
        co_await watcher.shutdown();
        EXPECT_EQ(watcher.project_subscription_count(), 0u);
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
