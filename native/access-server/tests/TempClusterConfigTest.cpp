#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "NacosSnapshotTestBuilder.h"
#include "NacosSubscriptionStub.h"
#include "config/AccessConfigCodec.h"
#include "runtime/TempClusterConfigWatcher.h"
#include "runtime/TempClusterStore.h"

namespace {

class FakeConfigService final : public fiber::nacos::ConfigService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>;

    fiber::common::IoResult<void> start() noexcept override { return {}; }
    fiber::async::Task<void> shutdown() noexcept override { co_return; }
    StatusSubscriber subscribe_status() override { return status_.subscribe(); }

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
        auto [iterator, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return iterator->second->subscriptions.subscribe(on_notify, ctx);
    }

    void push(std::string content, std::string md5 = {}) {
        const auto iterator = entries_.find(
                make_key(fiber::access_server::kTempClusterConfigDataId, fiber::access_server::kDefaultNacosGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::Present, std::move(md5),
                                                       std::move(content)),
        });
    }

    void close() {
        const auto iterator = entries_.find(
                make_key(fiber::access_server::kTempClusterConfigDataId, fiber::access_server::kDefaultNacosGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{.kind = fiber::nacos::ResultKind::Closed});
    }

private:
    struct Entry {
        fiber::tests::NacosSubscriptionStub<fiber::nacos::ConfigData> subscriptions;
    };

    static std::string make_key(std::string_view data_id, std::string_view group) {
        std::string key(data_id);
        key.push_back('\n');
        key.append(group);
        return key;
    }

    std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
    fiber::async::Watch<fiber::nacos::ConfigServiceStatus> status_{fiber::nacos::ConfigServiceStatus{}};
};

fiber::async::Task<void> yield_updates() {
    for (std::size_t i = 0; i < 8; ++i) {
        co_await fiber::async::yield();
    }
}

std::vector<std::string> clusters_for(const fiber::access_server::TempClusterConfig &config, std::string_view service) {
    for (const fiber::access_server::TempClusterConfigEntry &entry: config) {
        if (entry.service == service) {
            return entry.clusters;
        }
    }
    return {};
}

bool decide(const fiber::access_server::ProxyClusterFallback &fallback, std::string_view service,
            std::string_view configured, std::string_view requested,
            fiber::access_server::ProxyAddressSelectErrorCode error) noexcept {
    return fallback.should_fallback != nullptr &&
           fallback.should_fallback(fallback.context, service, configured, requested, error);
}

TEST(TempClusterConfigTest, DecodesJavaWhitelistMap) {
    auto decoded =
            fiber::access_server::parse_temp_cluster_config(R"({"orders":["gray","canary"],"payments":["temp-1"]})");
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_TRUE(decoded->has_value());
    EXPECT_EQ((**decoded).size(), 2u);
    EXPECT_EQ(clusters_for(**decoded, "orders"), (std::vector<std::string>{"gray", "canary"}));
    EXPECT_EQ(clusters_for(**decoded, "payments"), (std::vector<std::string>{"temp-1"}));
    EXPECT_TRUE(clusters_for(**decoded, "unknown").empty());
}

TEST(TempClusterConfigTest, EmptyAndNullContentAreAcceptedClears) {
    // Java maps empty and null content to an empty map, unlike the gray
    // configuration where empty content keeps the active rules.
    const auto empty = fiber::access_server::parse_temp_cluster_config("");
    ASSERT_TRUE(empty);
    ASSERT_TRUE(empty->has_value());
    EXPECT_TRUE((*empty)->empty());

    const auto null_value = fiber::access_server::parse_temp_cluster_config("null");
    ASSERT_TRUE(null_value);
    ASSERT_TRUE(null_value->has_value());
    EXPECT_TRUE((*null_value)->empty());

    const auto empty_object = fiber::access_server::parse_temp_cluster_config("{}");
    ASSERT_TRUE(empty_object);
    ASSERT_TRUE(empty_object->has_value());
    EXPECT_TRUE((*empty_object)->empty());
}

TEST(TempClusterConfigTest, RejectsInvalidPayloads) {
    EXPECT_FALSE(fiber::access_server::parse_temp_cluster_config("{"));
    EXPECT_FALSE(fiber::access_server::parse_temp_cluster_config(R"(["orders"])"));
    EXPECT_FALSE(fiber::access_server::parse_temp_cluster_config(R"({"orders":"gray"})"));
    EXPECT_FALSE(fiber::access_server::parse_temp_cluster_config(R"({"orders":[null]})"));
}

TEST(TempClusterConfigTest, StoreAppliesAndLooksUpSortedWhitelist) {
    fiber::access_server::TempClusterStore store;
    EXPECT_EQ(store.generation(), 0u);
    EXPECT_FALSE(store.contains("orders", "gray"));

    fiber::access_server::TempClusterConfig config{
            fiber::access_server::TempClusterConfigEntry{
                    .service = "orders",
                    .clusters = {"canary", "gray"},
            },
    };
    ASSERT_TRUE(store.apply(std::optional<fiber::access_server::TempClusterConfig>(std::move(config))));
    EXPECT_EQ(store.generation(), 1u);
    EXPECT_EQ(store.service_count(), 1u);
    EXPECT_TRUE(store.contains("orders", "gray"));
    EXPECT_TRUE(store.contains("orders", "canary"));
    EXPECT_FALSE(store.contains("orders", "stable"));
    EXPECT_FALSE(store.contains("payments", "gray"));

    ASSERT_TRUE(store.apply(
            std::optional<fiber::access_server::TempClusterConfig>(fiber::access_server::TempClusterConfig{})));
    EXPECT_EQ(store.generation(), 2u);
    EXPECT_FALSE(store.contains("orders", "gray"));
}

TEST(TempClusterConfigTest, TestDeciderFallsBackOnlyOnUnwhitelistedNoHosts) {
    fiber::access_server::TempClusterStore store;
    ASSERT_TRUE(
            store.apply(std::optional<fiber::access_server::TempClusterConfig>(fiber::access_server::TempClusterConfig{
                    fiber::access_server::TempClusterConfigEntry{
                            .service = "orders",
                            .clusters = {"temp-1"},
                    },
            })));
    const fiber::access_server::ProxyClusterFallback fallback = store.adapter();

    using fiber::access_server::ProxyAddressSelectErrorCode;
    // Whitelisted service cluster keeps the no-host failure (Java
    // nameClusterMap short-circuit).
    EXPECT_FALSE(decide(fallback, "orders", "stable", "temp-1", ProxyAddressSelectErrorCode::NoHosts));
    // Unlisted cluster falls back.
    EXPECT_TRUE(decide(fallback, "orders", "stable", "temp-2", ProxyAddressSelectErrorCode::NoHosts));
    // Unlisted service falls back.
    EXPECT_TRUE(decide(fallback, "payments", "stable", "temp-1", ProxyAddressSelectErrorCode::NoHosts));
    // Circuit-open selections keep their failure in test environments.
    EXPECT_FALSE(decide(fallback, "orders", "stable", "temp-2", ProxyAddressSelectErrorCode::CircuitOpen));
    EXPECT_FALSE(decide(fallback, "payments", "stable", "temp-2", ProxyAddressSelectErrorCode::CircuitOpen));
}

TEST(TempClusterConfigTest, ProductionDeciderFallsBackOnEveryFailure) {
    const fiber::access_server::ProxyClusterFallback fallback =
            fiber::access_server::make_unconditional_cluster_fallback();
    using fiber::access_server::ProxyAddressSelectErrorCode;
    EXPECT_TRUE(decide(fallback, "orders", "stable", "gray", ProxyAddressSelectErrorCode::NoHosts));
    EXPECT_TRUE(decide(fallback, "orders", "stable", "gray", ProxyAddressSelectErrorCode::CircuitOpen));
}

TEST(TempClusterConfigTest, WatcherAppliesRetainsAndClears) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::TempClusterStore store;
    fiber::access_server::TempClusterConfigWatcher watcher(loop, service, store);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        service.push(R"({"orders":["temp-1"]})", "v1");
        co_await yield_updates();
        EXPECT_EQ(watcher.successful_updates(), 1u);
        EXPECT_TRUE(store.contains("orders", "temp-1"));

        service.push(R"({"orders":{"cluster":"temp-1"}})", "invalid");
        co_await yield_updates();
        EXPECT_TRUE(store.contains("orders", "temp-1"));
        EXPECT_EQ(watcher.failed_updates(), 1u);
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->stage, "decode");
        }

        // Java maps empty content to an empty map: the whitelist clears.
        service.push("", "empty");
        co_await yield_updates();
        EXPECT_FALSE(store.contains("orders", "temp-1"));
        EXPECT_EQ(watcher.successful_updates(), 2u);

        service.close();
        co_await yield_updates();
        EXPECT_EQ(watcher.state(), fiber::access_server::TempClusterConfigWatcherState::Failed);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(watcher.state(), fiber::access_server::TempClusterConfigWatcherState::Stopped);
}

} // namespace
