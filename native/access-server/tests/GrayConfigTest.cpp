#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "NacosSnapshotTestBuilder.h"
#include "NacosSubscriptionStub.h"
#include "config/AccessConfigCodec.h"
#include "runtime/GrayConfigWatcher.h"

namespace {

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
        auto [iterator, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return iterator->second->subscriptions.subscribe(on_notify, ctx);
    }

    void push(std::string content, std::string md5 = {}) {
        const auto iterator = entries_.find(
                make_key(fiber::access_server::kGrayConfigDataId, fiber::access_server::kDefaultNacosGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::Present, std::move(md5),
                                                       std::move(content)),
        });
    }

    void close() {
        const auto iterator = entries_.find(
                make_key(fiber::access_server::kGrayConfigDataId, fiber::access_server::kDefaultNacosGroup));
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
};

fiber::async::Task<void> yield_updates() {
    for (std::size_t i = 0; i < 8; ++i) {
        co_await fiber::async::yield();
    }
}

fiber::access_server::ClientMetadata metadata_for(const fiber::net::IpAddress &address) {
    fiber::access_server::ClientMetadata metadata;
    metadata.client_address = address;
    metadata.gray_target = fiber::access_server::Cidr::from_address(address);
    return metadata;
}

std::uint64_t mix_expected_random(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

std::uint32_t next_expected_sample(std::uint64_t &sequence) noexcept {
    sequence += 0x9e3779b97f4a7c15ULL;
    return static_cast<std::uint32_t>(mix_expected_random(sequence) % 10000U);
}

std::vector<std::vector<std::uint8_t>> collect_worker_matches(fiber::event::EventLoopGroup &workers,
                                                              fiber::access_server::ProxyClusterMatcher matcher,
                                                              std::string_view entry,
                                                              const fiber::access_server::ClientMetadata &metadata,
                                                              std::size_t sample_count) {
    std::vector<std::vector<std::uint8_t>> results(workers.size(), std::vector<std::uint8_t>(sample_count));
    std::atomic<std::size_t> done{0};
    for (std::size_t worker = 0; worker < workers.size(); ++worker) {
        fiber::async::spawn(workers.at(worker), [&, worker]() -> fiber::async::DetachedTask {
            for (std::size_t sample = 0; sample < sample_count; ++sample) {
                results[worker][sample] = matcher.matches(matcher.context, entry, metadata) ? 1 : 0;
            }
            done.fetch_add(1, std::memory_order_acq_rel);
            done.notify_all();
            co_return;
        });
    }

    std::size_t completed = done.load(std::memory_order_acquire);
    while (completed != workers.size()) {
        done.wait(completed, std::memory_order_acquire);
        completed = done.load(std::memory_order_acquire);
    }
    return results;
}

TEST(GrayConfigTest, DecodesJavaMapAndAppliesRatioAndCidrRules) {
    auto decoded =
            fiber::access_server::parse_gray_match_config(R"({"vdi":{"ratio":1000,"cidrs":["10.0.0.0/8","bad"]},)"
                                                          R"("desktop":{"ratio":10001},"unknown":{"ratio":10000}})");
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(*decoded);
    ASSERT_EQ((*decoded)->size(), 3u);

    fiber::access_server::GrayMatchStore store;
    auto applied = store.apply(*decoded);
    ASSERT_TRUE(applied);
    EXPECT_EQ(store.rule_count(), 2u);
    EXPECT_EQ(store.generation(), 1u);

    const auto inside = metadata_for(fiber::net::IpAddress::v4({10, 1, 2, 3}));
    const auto outside = metadata_for(fiber::net::IpAddress::v4({192, 0, 2, 1}));
    EXPECT_TRUE(store.matches("vdi", inside, 9999));
    EXPECT_TRUE(store.matches("vdi", outside, 999));
    EXPECT_FALSE(store.matches("vdi", outside, 1000));
    EXPECT_TRUE(store.matches("desktop", outside, 9999));
    EXPECT_FALSE(store.matches("unknown", inside, 0));

    auto empty_wire = fiber::access_server::parse_gray_match_config("");
    ASSERT_TRUE(empty_wire);
    EXPECT_FALSE(*empty_wire);
    EXPECT_EQ(store.apply(*empty_wire), fiber::access_server::GrayMatchUpdateStatus::IgnoredEmpty);
    EXPECT_EQ(store.rule_count(), 2u);
    EXPECT_EQ(store.generation(), 1u);

    auto clear = fiber::access_server::parse_gray_match_config("null");
    ASSERT_TRUE(clear);
    ASSERT_TRUE(*clear);
    EXPECT_TRUE((*clear)->empty());
    EXPECT_EQ(store.apply(*clear), fiber::access_server::GrayMatchUpdateStatus::Published);
    EXPECT_EQ(store.rule_count(), 0u);
    EXPECT_EQ(store.generation(), 2u);
}

TEST(GrayConfigTest, AppliesEveryBasisPointRatioExactly) {
    auto decoded =
            fiber::access_server::parse_gray_match_config(R"({"vdi":{"ratio":1},"desktop":{"ratio":2500},)"
                                                          R"("internet":{"ratio":9999},"custom":{"ratio":10000}})");
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(*decoded);

    fiber::access_server::GrayMatchStore store;
    ASSERT_TRUE(store.apply(*decoded));
    const auto outside = metadata_for(fiber::net::IpAddress::v4({192, 0, 2, 1}));
    std::array<std::size_t, 4> matched{};
    constexpr std::array<std::string_view, 4> entries{"vdi", "desktop", "internet", "custom"};
    for (std::uint32_t sample = 0; sample < 10000; ++sample) {
        for (std::size_t entry = 0; entry < entries.size(); ++entry) {
            matched[entry] += store.matches(entries[entry], outside, sample) ? 1 : 0;
        }
    }
    EXPECT_EQ(matched, (std::array<std::size_t, 4>{1, 2500, 9999, 10000}));
}

TEST(GrayConfigTest, UsesDeterministicIndependentWorkerSnapshotsAndPrng) {
    constexpr std::uint64_t random_seed = 0x123456789abcdef0ULL;
    constexpr std::size_t sample_count = 64;
    auto half = fiber::access_server::parse_gray_match_config(R"({"custom":{"ratio":5000}})");
    auto all = fiber::access_server::parse_gray_match_config(R"({"custom":{"ratio":10000}})");
    auto clear = fiber::access_server::parse_gray_match_config("null");
    ASSERT_TRUE(half);
    ASSERT_TRUE(*half);
    ASSERT_TRUE(all);
    ASSERT_TRUE(*all);
    ASSERT_TRUE(clear);
    ASSERT_TRUE(*clear);
    const auto outside = metadata_for(fiber::net::IpAddress::v4({192, 0, 2, 1}));

    for (const std::size_t worker_count: std::array<std::size_t, 3>{1, 2, 4}) {
        fiber::event::EventLoopGroup workers(worker_count);
        fiber::access_server::GrayMatchStore store(
                workers, fiber::access_server::GrayMatchStoreOptions{.random_seed = random_seed});
        EXPECT_EQ(store.generation(), 0u);
        const fiber::access_server::ProxyClusterMatcher matcher = store.adapter();
        EXPECT_FALSE(matcher.matches(matcher.context, "custom", outside));
        workers.start();
        const auto initial = collect_worker_matches(workers, matcher, "custom", outside, 1);
        for (const auto &worker: initial) {
            EXPECT_EQ(worker, std::vector<std::uint8_t>(1, 0));
        }

        ASSERT_TRUE(store.apply(*half));
        EXPECT_EQ(store.generation(), 1u);

        const auto sampled = collect_worker_matches(workers, matcher, "custom", outside, sample_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            std::uint64_t sequence = mix_expected_random(random_seed ^ (0x9e3779b97f4a7c15ULL * (worker + 1)));
            (void) next_expected_sample(sequence); // The initial empty-snapshot
                                                   // request still consumes a sample.
            for (std::size_t sample = 0; sample < sample_count; ++sample) {
                EXPECT_EQ(sampled[worker][sample], next_expected_sample(sequence) < 5000U ? 1 : 0);
            }
        }
        if (worker_count > 1) {
            EXPECT_NE(sampled[0], sampled[1]);
        }

        ASSERT_TRUE(store.apply(*all));
        EXPECT_EQ(store.generation(), 2u);
        const auto fully_matched = collect_worker_matches(workers, matcher, "custom", outside, 8);
        for (const auto &worker: fully_matched) {
            EXPECT_EQ(worker, std::vector<std::uint8_t>(8, 1));
        }

        ASSERT_TRUE(store.apply(*clear));
        EXPECT_EQ(store.generation(), 3u);
        const auto cleared = collect_worker_matches(workers, matcher, "custom", outside, 8);
        for (const auto &worker: cleared) {
            EXPECT_EQ(worker, std::vector<std::uint8_t>(8, 0));
        }
        workers.stop();
        workers.join();
    }
}

TEST(GrayConfigTest, WatcherRetainsOnEmptyAndInvalidThenAcceptsClear) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::GrayMatchStore store;
    fiber::access_server::AccessActivationEvidenceStore activation_evidence(
            loop, fiber::access_server::AccessActivationEvidenceIdentity{});
    fiber::access_server::GrayConfigWatcher watcher(loop, service, store, {}, activation_evidence.gray_observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        service.push(R"({"internet":{"ratio":5000,"cidrs":["2001:db8::/32"]}})", "v1");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 1u);
        EXPECT_TRUE(store.matches(
                "internet",
                metadata_for(fiber::net::IpAddress::v6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})),
                9999));
        EXPECT_EQ(activation_evidence.pin()->gray.resource.active_md5, "v1");
        EXPECT_EQ(activation_evidence.pin()->gray.generation, 1U);

        std::string too_many_rules = "{";
        for (std::size_t index = 0; index <= fiber::access_server::kAccessConfigLimits.gray_rules.max_rules; ++index) {
            if (index != 0) {
                too_many_rules.push_back(',');
            }
            too_many_rules.append("\"rule");
            too_many_rules.append(std::to_string(index));
            too_many_rules.append("\":{}");
        }
        too_many_rules.push_back('}');
        service.push(std::move(too_many_rules), "limited");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 1u);
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->error.code, fiber::access_server::AccessConfigErrorCode::LimitExceeded);
        }
        EXPECT_EQ(activation_evidence.pin()->gray.resource.observed_md5, "limited");
        EXPECT_EQ(activation_evidence.pin()->gray.resource.active_md5, "v1");
        EXPECT_EQ(activation_evidence.pin()->gray.resource.candidate_status,
                  fiber::access_server::AccessActivationCandidateStatus::Rejected);

        service.push("", "empty");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 1u);

        service.push("{", "invalid");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 1u);
        EXPECT_EQ(watcher.failed_updates(), 2u);
        EXPECT_TRUE(watcher.last_failure());

        service.push("{}", "clear");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 0u);
        EXPECT_EQ(watcher.successful_updates(), 2u);
        EXPECT_EQ(activation_evidence.pin()->gray.resource.active_md5, "clear");
        EXPECT_EQ(activation_evidence.pin()->gray.generation, 2U);

        service.close();
        co_await yield_updates();
        EXPECT_EQ(watcher.state(), fiber::access_server::GrayConfigWatcherState::Failed);
        const auto closed_evidence = activation_evidence.pin();
        EXPECT_EQ(closed_evidence->gray.watcher_state, "failed");
        EXPECT_TRUE(closed_evidence->gray.resource.failure);
        if (closed_evidence->gray.resource.failure) {
            EXPECT_EQ(closed_evidence->gray.resource.failure->code, "subscription_closed");
        }
        EXPECT_EQ(closed_evidence->gray.resource.active_md5, "clear");

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(watcher.state(), fiber::access_server::GrayConfigWatcherState::Stopped);
}

} // namespace
