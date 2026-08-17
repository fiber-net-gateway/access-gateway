#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Watch.h>
#include <fiber/async/Yield.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "NacosSnapshotTestBuilder.h"
#include "NacosSubscriptionStub.h"
#include "observability/AccessConfigMetrics.h"
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

using ReadinessWatch = fiber::async::Watch<fiber::access_server::AccessConfigReadiness>;

fiber::async::Task<void> wait_for_readiness(ReadinessWatch::Subscriber &readiness, ReadinessWatch::Snapshot &snapshot,
                                            fiber::access_server::AccessConfigReadinessState state) {
    snapshot = readiness.current();
    while (!snapshot.value || snapshot.value->state != state ||
           (state == fiber::access_server::AccessConfigReadinessState::Ready &&
            snapshot.value->processing_projects != 0)) {
        snapshot = co_await readiness.next(snapshot.version);
    }
}

fiber::async::Task<void> wait_for_ready_to_publish(ReadinessWatch::Subscriber &readiness,
                                                   ReadinessWatch::Snapshot &snapshot, std::size_t count) {
    snapshot = readiness.current();
    while (!snapshot.value || snapshot.value->ready_to_publish_projects < count) {
        snapshot = co_await readiness.next(snapshot.version);
    }
}

struct ActivationEvidenceCapture {
    fiber::access_server::AccessRouteActivationEvidenceObserver downstream;
    std::string target_project_list_md5;
    std::string expected_remaining_project;
    bool observed_premature_project_list_activation = false;

    static void observe(void *context, const fiber::access_server::AccessRouteActivationEvidence &evidence) noexcept {
        auto &capture = *static_cast<ActivationEvidenceCapture *>(context);
        if (!capture.target_project_list_md5.empty() &&
            evidence.project_list.active_md5 == capture.target_project_list_md5) {
            for (const auto &project: evidence.projects) {
                if (project.name != capture.expected_remaining_project) {
                    capture.observed_premature_project_list_activation = true;
                }
            }
        }
        if (capture.downstream.on_update) {
            capture.downstream.on_update(capture.downstream.context, evidence);
        }
    }

    [[nodiscard]] fiber::access_server::AccessRouteActivationEvidenceObserver observer() noexcept {
        return {
                .context = this,
                .on_update = &observe,
        };
    }
};

std::string route_config(std::int32_t version, std::string_view host, std::string_view service) {
    return std::string("{\"version\":") + std::to_string(version) + ",\"host\":{\"" + std::string(host) +
           "\":{}},\"routes\":[{\"path\":\"/\",\"service\":\"" + std::string(service) + "\"}]}";
}

std::string conditional_route_config(std::int32_t version, std::string_view host, std::string_view service) {
    return std::string("{\"version\":") + std::to_string(version) + ",\"host\":{\"" + std::string(host) +
           "\":{}},\"routes\":[{\"path\":\"/"
           "\",\"condition\":\"true\",\"service\":\"" +
           std::string(service) + "\"}]}";
}

fiber::access_server::ProjectConfig stored_route_config(std::int32_t version, std::string host, std::string service) {
    fiber::access_server::RouteConfig route;
    route.path = "/";
    route.service = std::move(service);

    fiber::access_server::ProjectConfig config;
    config.version = version;
    config.hosts = std::vector<fiber::access_server::HostConfigEntry>{
            fiber::access_server::HostConfigEntry{
                    .pattern = std::move(host),
                    .strategy = fiber::access_server::HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<fiber::access_server::RouteConfig>>{std::move(route)};
    return config;
}

class DeferredServiceReadiness final {
public:
    explicit DeferredServiceReadiness(bool ready) : ready_(ready), wait_calls_(0U) {
        ready_publisher_ = ready_.acquire_publisher();
        wait_calls_publisher_ = wait_calls_.acquire_publisher();
        FIBER_ASSERT(ready_publisher_);
        FIBER_ASSERT(wait_calls_publisher_);
    }

    [[nodiscard]] bool ready() const noexcept {
        const auto snapshot = ready_.current();
        return snapshot.value && *snapshot.value;
    }

    [[nodiscard]] fiber::async::Task<std::expected<void, fiber::access_server::ProxyAddressReadyError>>
    wait_ready() noexcept {
        wait_calls_publisher_->publish(++observed_wait_calls_);
        auto subscriber = ready_.subscribe();
        auto snapshot = subscriber.current();
        while (!snapshot.value || !*snapshot.value) {
            snapshot = co_await subscriber.next(snapshot.version);
        }
        co_return std::expected<void, fiber::access_server::ProxyAddressReadyError>{};
    }

    [[nodiscard]] fiber::async::Task<void> wait_for_calls(std::size_t expected) {
        auto subscriber = wait_calls_.subscribe();
        auto snapshot = subscriber.current();
        while (!snapshot.value || *snapshot.value < expected) {
            snapshot = co_await subscriber.next(snapshot.version);
        }
    }

    void set_ready(bool ready) { ready_publisher_->publish(ready); }

private:
    fiber::async::Watch<bool> ready_;
    fiber::async::Watch<std::size_t> wait_calls_;
    std::optional<fiber::async::Watch<bool>::Publisher> ready_publisher_;
    std::optional<fiber::async::Watch<std::size_t>::Publisher> wait_calls_publisher_;
    std::size_t observed_wait_calls_ = 0;
};

class DeferredServiceAddressSelector final : public fiber::access_server::ProxyAddressSelector {
public:
    DeferredServiceAddressSelector(DeferredServiceReadiness &readiness, std::string service, std::string cluster) :
        readiness_(&readiness), service_(std::move(service)), cluster_(std::move(cluster)) {}

    [[nodiscard]] fiber::async::Task<std::expected<void, fiber::access_server::ProxyAddressReadyError>>
    wait_ready() noexcept override {
        return readiness_->wait_ready();
    }

    [[nodiscard]] bool ready_for_publish() const noexcept override { return readiness_->ready(); }

    std::expected<fiber::access_server::ProxyUpstreamEndpoint, fiber::access_server::ProxyAddressSelectError>
    select_address(std::optional<std::string_view>, std::span<const std::uint64_t>) noexcept override {
        return std::unexpected(fiber::access_server::ProxyAddressSelectError{
                .code = fiber::access_server::ProxyAddressSelectErrorCode::NoHosts,
                .io_error = fiber::common::IoErr::NotFound,
                .message = "test selector has no request addresses",
        });
    }

    [[nodiscard]] std::string_view service_name() const noexcept override { return service_; }

    [[nodiscard]] std::optional<std::string_view> configured_cluster() const noexcept override { return cluster_; }

private:
    DeferredServiceReadiness *readiness_ = nullptr;
    std::string service_;
    std::string cluster_;
};

struct DeferredServiceSelectorFactory {
    DeferredServiceReadiness cached{true};
    DeferredServiceReadiness slow{false};

    [[nodiscard]] fiber::access_server::ProxyAddressSelectorFactory adapter() noexcept {
        return fiber::access_server::ProxyAddressSelectorFactory{
                .context = this,
                .create_service = &create,
        };
    }

    static fiber::access_server::ProxyAddressSelectorFactory::Result create(void *context, std::string service,
                                                                            std::string cluster) {
        auto &factory = *static_cast<DeferredServiceSelectorFactory *>(context);
        DeferredServiceReadiness *readiness = nullptr;
        if (service == "cached") {
            readiness = &factory.cached;
        } else if (service == "slow") {
            readiness = &factory.slow;
        } else {
            return std::unexpected(fiber::access_server::ProxyAddressSelectorFactory::Error{
                    .field = "service",
                    .message = "unexpected test service",
            });
        }
        std::shared_ptr<fiber::access_server::ProxyAddressSelector> selector =
                std::make_shared<DeferredServiceAddressSelector>(*readiness, std::move(service), std::move(cluster));
        return selector;
    }
};

TEST(AccessConfigWatcherTest, ReconcilesProjectsAndRetainsLastValidSnapshots) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::AccessConfigMetrics config_metrics(loop);
    fiber::access_server::AccessActivationEvidenceStore activation_evidence(
            loop, fiber::access_server::AccessActivationEvidenceIdentity{});
    ActivationEvidenceCapture activation_capture{
            .downstream = activation_evidence.route_observer(),
    };
    std::size_t observer_updates = 0;
    fiber::access_server::RouteSnapshotObserver observer{
            .context = &observer_updates,
            .on_update =
                    [](void *context, std::shared_ptr<const fiber::access_server::AccessRouteSnapshot>) noexcept {
                        ++*static_cast<std::size_t *>(context);
                    },
    };
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store, {}, observer,
                                                      config_metrics.observer(), activation_capture.observer());
    bool completed = false;

    compiler_group.start();

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
        co_await wait_for_readiness(readiness, readiness_snapshot,
                                    fiber::access_server::AccessConfigReadinessState::Ready);
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
        EXPECT_EQ(readiness_snapshot.value->synchronized_projects, 2u);
        EXPECT_EQ(readiness_snapshot.value->rejected_projects, 0u);
        EXPECT_EQ(observer_updates, 1u);
        EXPECT_TRUE(store.pin()->match_host("a.example.com"));
        EXPECT_TRUE(store.pin()->match_host("b.example.com"));
        const auto valid = store.pin();
        auto evidence = activation_evidence.pin();
        EXPECT_EQ(evidence->route.projects.size(), 2U);
        EXPECT_EQ(evidence->route.snapshot_generation, 1U);
        if (evidence->route.projects.size() == 2U) {
            EXPECT_EQ(evidence->route.projects[0].name, "a");
            EXPECT_EQ(evidence->route.projects[0].active_md5, "a1");
            EXPECT_EQ(evidence->route.projects[0].active_version, 1);
            EXPECT_TRUE(evidence->route.projects[0].active_loaded);
            EXPECT_EQ(evidence->route.projects[1].name, "b");
            EXPECT_EQ(evidence->route.projects[1].active_md5, "b1");
        }

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
        co_await wait_for_readiness(readiness, readiness_snapshot,
                                    fiber::access_server::AccessConfigReadinessState::Ready);
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
        evidence = activation_evidence.pin();
        EXPECT_EQ(evidence->route.projects.size(), 2U);
        if (evidence->route.projects.size() == 2U) {
            EXPECT_EQ(evidence->route.projects[0].observed_md5, "same");
            EXPECT_EQ(evidence->route.projects[0].active_md5, "a1");
            EXPECT_EQ(evidence->route.projects[0].candidate_status,
                      fiber::access_server::AccessActivationCandidateStatus::Accepted);
            EXPECT_EQ(evidence->route.projects[1].observed_md5, "invalid");
            EXPECT_EQ(evidence->route.projects[1].active_md5, "b1");
            EXPECT_EQ(evidence->route.projects[1].candidate_status,
                      fiber::access_server::AccessActivationCandidateStatus::Rejected);
            EXPECT_TRUE(evidence->route.projects[1].failure);
        }

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

        activation_capture.target_project_list_md5 = "only-c";
        activation_capture.expected_remaining_project = "c";
        service.push(fiber::access_server::kProjectListDataId, "c", "only-c");
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 1u);
        EXPECT_FALSE(store.pin()->match_host("a.example.com"));
        EXPECT_FALSE(store.pin()->match_host("b.example.com"));
        EXPECT_FALSE(activation_capture.observed_premature_project_list_activation);

        service.push_not_found(fiber::access_server::kProjectListDataId);
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 0u);
        EXPECT_TRUE(store.pin()->projects().empty());
        EXPECT_GE(observer_updates, 4u);
        readiness_snapshot = readiness.current();
        EXPECT_TRUE(readiness_snapshot.value);
        EXPECT_EQ(readiness_snapshot.value->state, fiber::access_server::AccessConfigReadinessState::Ready);
        EXPECT_EQ(readiness_snapshot.value->desired_projects, 0u);
        EXPECT_TRUE(activation_evidence.pin()->route.projects.empty());

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    compiler_group.stop();
    compiler_group.join();
    EXPECT_TRUE(completed);
    EXPECT_EQ(watcher.state(), fiber::access_server::AccessConfigWatcherState::Stopped);
    std::string metrics;
    config_metrics.append_prometheus(metrics, std::chrono::steady_clock::now());
    EXPECT_NE(metrics.find("access_server_config_updates_total{resource="
                           "\"project_list\",result=\"success\",reason="
                           "\"accepted\"} 5"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_updates_total{resource="
                           "\"project_list\",result=\"failure\",reason="
                           "\"decode\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_updates_total{resource="
                           "\"project_route\",result=\"success\",reason="
                           "\"published\"} 2"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_updates_total{resource="
                           "\"project_route\",result=\"ignored\",reason="
                           "\"version_unchanged\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_updates_total{resource="
                           "\"project_route\",result=\"ignored\",reason="
                           "\"empty\"} 2"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_updates_total{resource="
                           "\"project_route\",result=\"success\",reason="
                           "\"removed\"} 3"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_updates_total{resource="
                           "\"project_route\",result=\"failure\",reason="
                           "\"decode\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_stage_duration_observations_"
                           "total{stage=\"project_compile\"} 4"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_stage_duration_observations_"
                           "total{stage=\"service_ready\"} 2"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_stage_duration_observations_"
                           "total{stage=\"global_build\"} 4"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_stage_duration_observations_"
                           "total{stage=\"publish\"} 4"),
              std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_readiness{state=\"stopped\"} 1"), std::string::npos);
    EXPECT_NE(metrics.find("access_server_config_projects{state=\"ready_to_publish\"} 0"), std::string::npos);
    EXPECT_NE(metrics.find("access_server_route_snapshot_resources{resource=\"project\"} 0"), std::string::npos);
    EXPECT_EQ(metrics.find("ploto.unified-access"), std::string::npos);
    EXPECT_EQ(metrics.find("example.com"), std::string::npos);
}

TEST(AccessConfigWatcherTest, InitialBatchIsolatesHostConflictsAndPublishesOnce) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
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
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store, {}, observer);
    bool completed = false;

    compiler_group.start();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        auto snapshot = readiness.current();
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "right;valid;left");
        service.push("ploto.unified-access.route.right", route_config(1, "SHARED.example.com", "right"));
        service.push("ploto.unified-access.route.valid", route_config(1, "valid.example.com", "valid"));
        service.push("ploto.unified-access.route.left", route_config(1, "shared.EXAMPLE.com", "left"));

        co_await wait_for_readiness(readiness, snapshot, fiber::access_server::AccessConfigReadinessState::Ready);

        EXPECT_EQ(observer_updates, 1U);
        EXPECT_EQ(watcher.successful_updates(), 2U);
        EXPECT_EQ(watcher.failed_updates(), 1U);
        EXPECT_EQ(store.pin()->projects().size(), 2U);
        const auto shared = store.pin()->match_host("shared.example.com");
        EXPECT_TRUE(shared);
        if (shared) {
            EXPECT_EQ(shared.project->project(), "left");
        }
        EXPECT_TRUE(store.pin()->match_host("valid.example.com"));
        const auto left = watcher.project_status("left");
        const auto right = watcher.project_status("right");
        const auto valid = watcher.project_status("valid");
        EXPECT_TRUE(left);
        EXPECT_TRUE(right);
        EXPECT_TRUE(valid);
        if (left) {
            EXPECT_EQ(left->config_state, fiber::access_server::AccessProjectConfigState::Accepted);
        }
        if (valid) {
            EXPECT_EQ(valid->config_state, fiber::access_server::AccessProjectConfigState::Accepted);
        }
        if (right) {
            EXPECT_EQ(right->config_state, fiber::access_server::AccessProjectConfigState::Rejected);
            EXPECT_TRUE(right->last_failure);
            if (right->last_failure) {
                EXPECT_EQ(right->last_failure->stage, fiber::access_server::AccessConfigWatcherFailureStage::Publish);
                EXPECT_EQ(right->last_failure->error.field, "host");
            }
        }

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    compiler_group.stop();
    compiler_group.join();
    EXPECT_TRUE(completed);
}

TEST(AccessConfigWatcherTest, InitialBatchDropsReplacedAndRemovedStagedCandidates) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
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
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store, {}, observer);
    bool completed = false;

    compiler_group.start();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        auto snapshot = readiness.current();
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "a;b;c");
        service.push("ploto.unified-access.route.a", route_config(1, "a-v1.example.com", "a"), "a-v1");
        service.push("ploto.unified-access.route.c", route_config(1, "c.example.com", "c"), "c-v1");
        co_await wait_for_ready_to_publish(readiness, snapshot, 2);
        const auto staged_a = watcher.project_status("a");
        const auto staged_c = watcher.project_status("c");
        EXPECT_TRUE(staged_a);
        EXPECT_TRUE(staged_c);
        if (staged_a) {
            EXPECT_EQ(staged_a->config_state, fiber::access_server::AccessProjectConfigState::ReadyToPublish);
        }
        if (staged_c) {
            EXPECT_EQ(staged_c->config_state, fiber::access_server::AccessProjectConfigState::ReadyToPublish);
        }
        EXPECT_TRUE(store.pin()->projects().empty());

        service.push("ploto.unified-access.route.a", route_config(2, "a-v2.example.com", "a"), "a-v2");
        service.push(fiber::access_server::kProjectListDataId, "a;b");
        service.push("ploto.unified-access.route.b", route_config(1, "b.example.com", "b"), "b-v1");
        co_await wait_for_readiness(readiness, snapshot, fiber::access_server::AccessConfigReadinessState::Ready);

        EXPECT_EQ(observer_updates, 1U);
        EXPECT_EQ(watcher.successful_updates(), 2U);
        EXPECT_EQ(store.current_version("a"), 2);
        EXPECT_EQ(store.current_version("b"), 1);
        EXPECT_FALSE(store.current_version("c"));
        EXPECT_FALSE(store.pin()->match_host("a-v1.example.com"));
        EXPECT_TRUE(store.pin()->match_host("a-v2.example.com"));
        EXPECT_TRUE(store.pin()->match_host("b.example.com"));
        EXPECT_FALSE(store.pin()->match_host("c.example.com"));
        EXPECT_FALSE(watcher.project_status("c"));

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    compiler_group.stop();
    compiler_group.join();
    EXPECT_TRUE(completed);
}

TEST(AccessConfigWatcherTest, InitialBatchWaitsForCurrentServiceReadinessAcrossArrivalOrders) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
    FakeConfigService service;
    DeferredServiceSelectorFactory selector_factory;
    fiber::access_server::RouteConfigStore store({}, selector_factory.adapter());
    std::size_t observer_updates = 0;
    fiber::access_server::RouteSnapshotObserver observer{
            .context = &observer_updates,
            .on_update =
                    [](void *context, std::shared_ptr<const fiber::access_server::AccessRouteSnapshot>) noexcept {
                        ++*static_cast<std::size_t *>(context);
                    },
    };
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store, {}, observer);
    bool completed = false;

    service.prime("ploto.unified-access.route.cached", route_config(1, "cached.example.com", "cached"), "cached-v1");
    compiler_group.start();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        auto snapshot = readiness.current();
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "cached;slow");

        co_await wait_for_ready_to_publish(readiness, snapshot, 1U);
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::SynchronizingProjects);
            EXPECT_EQ(snapshot.value->desired_projects, 2U);
            EXPECT_EQ(snapshot.value->subscribed_projects, 2U);
            EXPECT_EQ(snapshot.value->ready_to_publish_projects, 1U);
            EXPECT_EQ(snapshot.value->synchronized_projects, 0U);
        }
        EXPECT_TRUE(store.pin()->projects().empty());
        EXPECT_EQ(observer_updates, 0U);

        service.push("ploto.unified-access.route.slow", route_config(1, "slow-v1.example.com", "slow"), "slow-v1");
        co_await selector_factory.slow.wait_for_calls(1U);
        auto status = watcher.project_status("slow");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Processing);
            EXPECT_EQ(status->generation, 1U);
            EXPECT_EQ(status->observed_md5, "slow-v1");
        }
        EXPECT_TRUE(store.pin()->projects().empty());

        service.push("ploto.unified-access.route.slow", route_config(2, "slow-v2.example.com", "slow"), "slow-v2");
        co_await selector_factory.slow.wait_for_calls(2U);
        status = watcher.project_status("slow");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Processing);
            EXPECT_EQ(status->generation, 2U);
            EXPECT_EQ(status->observed_md5, "slow-v2");
        }
        EXPECT_TRUE(store.pin()->projects().empty());
        EXPECT_EQ(observer_updates, 0U);

        selector_factory.slow.set_ready(true);
        co_await wait_for_readiness(readiness, snapshot, fiber::access_server::AccessConfigReadinessState::Ready);

        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->desired_projects, 2U);
            EXPECT_EQ(snapshot.value->subscribed_projects, 2U);
            EXPECT_EQ(snapshot.value->synchronized_projects, 2U);
            EXPECT_EQ(snapshot.value->processing_projects, 0U);
            EXPECT_EQ(snapshot.value->ready_to_publish_projects, 0U);
        }
        EXPECT_EQ(observer_updates, 1U);
        EXPECT_EQ(watcher.successful_updates(), 2U);
        EXPECT_EQ(watcher.failed_updates(), 0U);
        EXPECT_EQ(store.current_version("cached"), 1);
        EXPECT_EQ(store.current_version("slow"), 2);
        EXPECT_TRUE(store.pin()->match_host("cached.example.com"));
        EXPECT_FALSE(store.pin()->match_host("slow-v1.example.com"));
        EXPECT_TRUE(store.pin()->match_host("slow-v2.example.com"));
        status = watcher.project_status("slow");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Accepted);
            EXPECT_EQ(status->observed_version, 2);
            EXPECT_EQ(status->published_generation, 2U);
        }

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    compiler_group.stop();
    compiler_group.join();
    EXPECT_TRUE(completed);
}

TEST(AccessConfigWatcherTest, KeepsOwnerLoopResponsiveAndCoalescesQueuedGenerations) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store);
    bool owner_progressed = false;
    bool compiler_started = false;
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        auto snapshot = readiness.current();
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "orders");
        service.push("ploto.unified-access.route.orders", route_config(1, "v1.example.com", "orders"), "v1");
        service.push("ploto.unified-access.route.orders", route_config(2, "v2.example.com", "orders"), "v2");
        service.push("ploto.unified-access.route.orders", conditional_route_config(3, "v3.example.com", "orders"),
                     "v3");

        const auto queued = watcher.project_status("orders");
        EXPECT_TRUE(queued);
        if (queued) {
            EXPECT_EQ(queued->config_state, fiber::access_server::AccessProjectConfigState::Processing);
            EXPECT_EQ(queued->generation, 3u);
            EXPECT_EQ(queued->observed_md5, "v3");
        }
        EXPECT_FALSE(store.current_version("orders"));
        owner_progressed = true;

        compiler_group.start();
        compiler_started = true;
        co_await wait_for_readiness(readiness, snapshot, fiber::access_server::AccessConfigReadinessState::Ready);

        const auto published = watcher.project_status("orders");
        EXPECT_TRUE(published);
        if (published) {
            EXPECT_EQ(published->config_state, fiber::access_server::AccessProjectConfigState::Accepted);
            EXPECT_EQ(published->observed_version, 3);
            EXPECT_EQ(published->published_generation, 3u);
        }
        EXPECT_EQ(store.current_version("orders"), 3);
        EXPECT_TRUE(store.pin()->match_host("v3.example.com"));
        EXPECT_FALSE(store.pin()->match_host("v1.example.com"));
        EXPECT_FALSE(store.pin()->match_host("v2.example.com"));
        EXPECT_EQ(store.pin()->projects().size(), 1u);
        if (!store.pin()->projects().empty()) {
            EXPECT_EQ(store.pin()->projects().front()->compiled_program_count(), 1u);
        }
        EXPECT_EQ(watcher.successful_updates(), 1u);
        EXPECT_EQ(watcher.failed_updates(), 0u);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    if (compiler_started) {
        compiler_group.stop();
        compiler_group.join();
    }
    EXPECT_TRUE(owner_progressed);
    EXPECT_TRUE(completed);
}

TEST(AccessConfigWatcherTest, ClosedProjectSubscriptionCancelsStaleCompileAndRecoversAfterReconcile) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    auto seeded = store.apply("orders", stored_route_config(1, "v1.example.com", "orders"));
    ASSERT_TRUE(seeded);
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store);
    bool compiler_started = false;
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "orders");
        service.push("ploto.unified-access.route.orders", route_config(2, "v2.example.com", "orders"), "v2");

        auto status = watcher.project_status("orders");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->subscription_state, fiber::access_server::AccessProjectSubscriptionState::Subscribed);
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Processing);
            EXPECT_EQ(status->generation, 1U);
            EXPECT_EQ(status->observed_md5, "v2");
        }

        service.close("ploto.unified-access.route.orders");

        status = watcher.project_status("orders");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->subscription_state, fiber::access_server::AccessProjectSubscriptionState::Failed);
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::AwaitingValue);
            EXPECT_FALSE(status->first_value_received);
            EXPECT_FALSE(status->synchronized);
            EXPECT_EQ(status->retry_attempt, 0U);
            EXPECT_EQ(status->next_retry_at, std::chrono::steady_clock::time_point{});
            EXPECT_EQ(status->generation, 2U);
            EXPECT_TRUE(status->last_failure);
            if (status->last_failure) {
                EXPECT_EQ(status->last_failure->stage,
                          fiber::access_server::AccessConfigWatcherFailureStage::Subscription);
                EXPECT_EQ(status->last_failure->io_error, fiber::common::IoErr::NotConnected);
            }
        }
        EXPECT_EQ(watcher.active_project_subscription_count(), 0U);

        auto snapshot = readiness.current();
        EXPECT_TRUE(snapshot.value);
        if (snapshot.value) {
            EXPECT_EQ(snapshot.value->state, fiber::access_server::AccessConfigReadinessState::SynchronizingProjects);
            EXPECT_EQ(snapshot.value->subscribed_projects, 0U);
            EXPECT_EQ(snapshot.value->synchronized_projects, 0U);
            EXPECT_EQ(snapshot.value->retrying_projects, 0U);
        }

        co_await yield_updates();
        EXPECT_EQ(service.subscribe_attempts("ploto.unified-access.route.orders"), 1U);
        EXPECT_EQ(store.current_version("orders"), 1);
        EXPECT_TRUE(store.pin()->match_host("v1.example.com"));
        EXPECT_FALSE(store.pin()->match_host("v2.example.com"));

        service.prime("ploto.unified-access.route.orders", route_config(3, "v3.example.com", "orders"), "v3");
        service.push_not_found(fiber::access_server::kProjectListDataId);
        EXPECT_FALSE(watcher.project_status("orders"));
        EXPECT_FALSE(store.current_version("orders"));
        service.push(fiber::access_server::kProjectListDataId, "orders");

        status = watcher.project_status("orders");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->subscription_state, fiber::access_server::AccessProjectSubscriptionState::Subscribed);
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Processing);
            EXPECT_EQ(status->generation, 1U);
            EXPECT_EQ(status->observed_md5, "v3");
        }
        EXPECT_EQ(service.subscribe_attempts("ploto.unified-access.route.orders"), 2U);

        compiler_group.start();
        compiler_started = true;
        co_await wait_for_readiness(readiness, snapshot, fiber::access_server::AccessConfigReadinessState::Ready);

        status = watcher.project_status("orders");
        EXPECT_TRUE(status);
        if (status) {
            EXPECT_EQ(status->subscription_state, fiber::access_server::AccessProjectSubscriptionState::Subscribed);
            EXPECT_EQ(status->config_state, fiber::access_server::AccessProjectConfigState::Accepted);
            EXPECT_EQ(status->observed_version, 3);
            EXPECT_EQ(status->published_generation, 1U);
        }
        EXPECT_EQ(store.current_version("orders"), 3);
        EXPECT_FALSE(store.pin()->match_host("v1.example.com"));
        EXPECT_FALSE(store.pin()->match_host("v2.example.com"));
        EXPECT_TRUE(store.pin()->match_host("v3.example.com"));
        EXPECT_EQ(watcher.successful_updates(), 2U);
        EXPECT_EQ(watcher.failed_updates(), 1U);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    if (compiler_started) {
        compiler_group.stop();
        compiler_group.join();
    }
    EXPECT_TRUE(completed);
}

TEST(AccessConfigWatcherTest, RetriesTransientProjectSubscriptionsAndReportsTypedReadiness) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
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
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store, options);
    bool completed = false;

    compiler_group.start();

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

        co_await wait_for_readiness(readiness, snapshot, fiber::access_server::AccessConfigReadinessState::Ready);
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
        co_await wait_for_readiness(readiness, snapshot, fiber::access_server::AccessConfigReadinessState::Ready);
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
    compiler_group.stop();
    compiler_group.join();
    EXPECT_TRUE(completed);
}

TEST(AccessConfigWatcherTest, ShutdownWinsAgainstQueuedConfigCallbacks) {
    fiber::event::EventLoop loop;
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::access_server::AccessConfigCompiler compiler(compiler_group.at(0));
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
    fiber::access_server::AccessConfigWatcher watcher(loop, compiler, service, store, options);
    bool completed = false;

    compiler_group.start();

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
    compiler_group.stop();
    compiler_group.join();
    EXPECT_TRUE(completed);
}

} // namespace
