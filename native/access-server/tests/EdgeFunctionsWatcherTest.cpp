#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Watch.h>
#include <fiber/async/Yield.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>
#include <openssl/sha.h>

#include "NacosSnapshotTestBuilder.h"
#include "NacosSubscriptionStub.h"
#include "config/AccessConfig.h"
#include "config/AccessConfigLimits.h"
#include "observability/AccessActivationEvidence.h"
#include "observability/AccessConfigMetrics.h"
#include "runtime/EdgeFunctionsWatcher.h"
#include "runtime/RouteConfigStore.h"

namespace {

// SHA-256 of the exact bytes the watcher verifies, hex-encoded like the index
// declares it.
std::string sha256_hex(std::string_view content) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    (void) SHA256(reinterpret_cast<const unsigned char *>(content.data()), content.size(), digest.data());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2U);
    for (const unsigned char byte: digest) {
        hex.push_back(kHex[byte >> 4U]);
        hex.push_back(kHex[byte & 0x0FU]);
    }
    return hex;
}

// Same fake the config/tls watcher tests use: one stub per Data ID, keyed on
// the ACCESS-SERVER group the watchers default to.
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
    fiber::async::Watch<fiber::nacos::ConfigServiceStatus> status_{fiber::nacos::ConfigServiceStatus{}};
};

fiber::async::Task<void> yield_updates() {
    for (std::size_t i = 0; i < 8; ++i) {
        co_await fiber::async::yield();
    }
}

using ReadinessWatch = fiber::async::Watch<bool>;

fiber::async::Task<void> wait_for_readiness(ReadinessWatch::Subscriber &readiness, ReadinessWatch::Snapshot &snapshot,
                                            bool value) {
    snapshot = readiness.current();
    while (!snapshot.value || *snapshot.value != value) {
        snapshot = co_await readiness.next(snapshot.version);
    }
}

struct MetricsCapture {
    std::vector<fiber::access_server::AccessConfigMetricEvent> events;

    static void observe(void *context, fiber::access_server::AccessConfigMetricEvent event) noexcept {
        static_cast<MetricsCapture *>(context)->events.push_back(event);
    }

    [[nodiscard]] bool saw(fiber::access_server::AccessConfigMetricEvent event) const {
        return std::find(events.begin(), events.end(), event) != events.end();
    }

    [[nodiscard]] fiber::access_server::AccessConfigMetricsObserver observer() noexcept {
        return {
                .context = this,
                .on_event = &observe,
        };
    }
};

struct EvidenceCapture {
    std::vector<fiber::access_server::AccessEdgeFunctionsActivationEvidence> updates;

    static void observe(void *context,
                        const fiber::access_server::AccessEdgeFunctionsActivationEvidence &evidence) noexcept {
        static_cast<EvidenceCapture *>(context)->updates.push_back(evidence);
    }

    [[nodiscard]] const fiber::access_server::AccessEdgeFunctionsActivationEvidence *last() const {
        return updates.empty() ? nullptr : &updates.back();
    }

    [[nodiscard]] fiber::access_server::AccessEdgeFunctionsActivationEvidenceObserver observer() noexcept {
        return {
                .context = this,
                .on_update = &observe,
        };
    }
};

// Wire helpers mirroring the codec fixtures: the script Data ID is derived
// from the slug name (ploto.unified-access.edge-function.<name>).
std::string function_json(const std::string &name, const std::string &sha256) {
    return "{\"name\": \"" + name + "\", \"sha256\": \"" + sha256 +
           "\", \"mounts\": [{\"projects\": {\"include\": [\"*\"]}}]}";
}

std::string index_json(std::string functions) {
    return "{\"version\": 1, \"functions\": [" + std::move(functions) + "]}";
}

std::string script_data_id(std::string_view id) {
    return std::string(fiber::access_server::kEdgeFunctionDataIdPrefix) + std::string(id);
}

// A script that compiles under the edge-function library and one that never
// does ($context is a route-only namespace).
constexpr std::string_view kGoodScript = "$header.x_edge_token == 's3cret'";
constexpr std::string_view kBadScript = "$context.request_id == 'nope'";

// Coroutine bodies may only use EXPECT_* (fatal asserts expand to `return`),
// so the pointer-shaped checks live in these plain functions.
void expect_readiness_value(const ReadinessWatch::Snapshot &snapshot, bool value) {
    ASSERT_TRUE(snapshot.value);
    if (snapshot.value) {
        EXPECT_EQ(*snapshot.value, value);
    }
}

void expect_failure(const std::optional<fiber::access_server::EdgeFunctionsWatcherFailure> &failure,
                    std::string_view stage, std::string_view code) {
    ASSERT_TRUE(failure.has_value());
    if (failure) {
        EXPECT_EQ(failure->stage, stage);
        EXPECT_EQ(failure->code, code);
    }
}

struct EvidenceExpectation {
    std::optional<std::string_view> watcher_state;
    std::optional<fiber::access_server::AccessActivationCandidateStatus> candidate_status;
    std::optional<std::string_view> observed_md5;
    std::optional<std::string_view> active_md5;
    std::optional<std::uint64_t> generation;
    std::optional<std::size_t> function_count;
    std::optional<std::size_t> total_script_bytes;
    std::optional<std::string_view> failure_code;
    bool no_failure = false;
};

void expect_evidence(const EvidenceCapture &capture, const EvidenceExpectation &expectation) {
    ASSERT_NE(capture.last(), nullptr);
    if (capture.last() == nullptr) {
        return;
    }
    const fiber::access_server::AccessEdgeFunctionsActivationEvidence &evidence = *capture.last();
    if (expectation.watcher_state) {
        EXPECT_EQ(evidence.watcher_state, *expectation.watcher_state);
    }
    if (expectation.candidate_status) {
        EXPECT_EQ(evidence.index.candidate_status, *expectation.candidate_status);
    }
    if (expectation.observed_md5) {
        EXPECT_EQ(evidence.index.observed_md5, *expectation.observed_md5);
    }
    if (expectation.active_md5) {
        EXPECT_EQ(evidence.index.active_md5, *expectation.active_md5);
    }
    if (expectation.generation) {
        EXPECT_EQ(evidence.generation, *expectation.generation);
    }
    if (expectation.function_count) {
        EXPECT_EQ(evidence.function_count, *expectation.function_count);
    }
    if (expectation.total_script_bytes) {
        EXPECT_EQ(evidence.total_script_bytes, *expectation.total_script_bytes);
    }
    if (expectation.failure_code) {
        ASSERT_TRUE(evidence.index.failure.has_value());
        if (evidence.index.failure) {
            EXPECT_EQ(evidence.index.failure->code, *expectation.failure_code);
        }
    }
    if (expectation.no_failure) {
        EXPECT_FALSE(evidence.index.failure.has_value());
    }
}

TEST(EdgeFunctionsWatcherTest, NotFoundIndexWithoutActiveSetIsQuiet) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    EvidenceCapture evidence;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer(),
                                                       evidence.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());
        EXPECT_EQ(watcher.state(), fiber::access_server::EdgeFunctionsWatcherState::Running);
        EXPECT_EQ(service.subscriptions(fiber::access_server::kEdgeFunctionIndexDataId), 1u);

        service.push_not_found(fiber::access_server::kEdgeFunctionIndexDataId);
        co_await yield_updates();
        EXPECT_EQ(watcher.failed_updates(), 0u);
        EXPECT_EQ(watcher.successful_updates(), 0u);
        EXPECT_FALSE(watcher.last_failure().has_value());
        EXPECT_EQ(watcher.script_subscription_count(), 0u);
        EXPECT_EQ(store.pin()->edge_function_count(), 0u);
        expect_readiness_value(readiness.current(), false);
        EXPECT_TRUE(metrics.events.empty());
        expect_evidence(evidence,
                        {
                                .watcher_state = "running",
                                .candidate_status = fiber::access_server::AccessActivationCandidateStatus::Awaiting,
                                .no_failure = true,
                        });

        co_await watcher.shutdown();
        EXPECT_EQ(watcher.state(), fiber::access_server::EdgeFunctionsWatcherState::Stopped);
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, AssemblesAndPublishesVerifiedSet) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    EvidenceCapture evidence;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer(),
                                                       evidence.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        // Prime the script before the index arrives: the replay fires during
        // reconcile's subscribe() and must be safe to apply reentrantly.
        const std::string script(kGoodScript);
        const std::string sha = sha256_hex(script);
        const std::string index = index_json(function_json("waf", sha));
        service.prime(script_data_id("waf"), script);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, index, "idx-1");
        co_await yield_updates();

        EXPECT_EQ(store.pin()->edge_function_count(), 1u);
        EXPECT_EQ(store.pin()->edge_function_script_bytes(), script.size());
        EXPECT_EQ(watcher.successful_updates(), 1u);
        EXPECT_EQ(watcher.failed_updates(), 0u);
        EXPECT_EQ(watcher.compiled_cache_size(), 1u);
        EXPECT_EQ(watcher.script_subscription_count(), 1u);
        EXPECT_FALSE(watcher.last_failure().has_value());
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionSetPublished));
        EXPECT_FALSE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionSetRemoved));
        expect_readiness_value(readiness.current(), true);
        expect_evidence(evidence,
                        {
                                .watcher_state = "running",
                                .candidate_status = fiber::access_server::AccessActivationCandidateStatus::Accepted,
                                .observed_md5 = "idx-1",
                                .active_md5 = "idx-1",
                                .generation = 1u,
                                .function_count = 1u,
                                .total_script_bytes = script.size(),
                                .no_failure = true,
                        });

        co_await watcher.shutdown();
        EXPECT_EQ(watcher.state(), fiber::access_server::EdgeFunctionsWatcherState::Stopped);
        EXPECT_EQ(watcher.script_subscription_count(), 0u);
        EXPECT_EQ(service.subscribe_attempts(script_data_id("waf")), 1u); // one-shot subscribe, no churn
        // Shutdown stops subscriptions but never unmounts the serving set.
        EXPECT_EQ(store.pin()->edge_function_count(), 1u);
        expect_evidence(evidence, {.watcher_state = "stopped"});
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, AllOrNothingUntilEveryScriptVerified) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script_a = "$header.a == '1'";
        const std::string script_b = "$header.b == '2'";
        service.push(
                fiber::access_server::kEdgeFunctionIndexDataId,
                index_json(function_json("a", sha256_hex(script_a)) + "," + function_json("b", sha256_hex(script_b))),
                "idx-1");
        co_await yield_updates();
        EXPECT_EQ(watcher.script_subscription_count(), 2u);
        EXPECT_EQ(store.pin()->edge_function_count(), 0u);
        EXPECT_EQ(watcher.successful_updates(), 0u);
        EXPECT_EQ(watcher.failed_updates(), 0u);

        service.push(script_data_id("a"), script_a);
        co_await yield_updates();
        // One verified script of two: nothing assembles yet.
        EXPECT_EQ(store.pin()->edge_function_count(), 0u);
        EXPECT_EQ(watcher.successful_updates(), 0u);
        EXPECT_EQ(watcher.compiled_cache_size(), 0u);
        EXPECT_FALSE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionSetPublished));

        service.push(script_data_id("b"), script_b);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        EXPECT_EQ(store.pin()->edge_function_count(), 2u);
        EXPECT_EQ(store.pin()->edge_function_script_bytes(), script_a.size() + script_b.size());
        EXPECT_EQ(watcher.successful_updates(), 1u);
        EXPECT_EQ(watcher.compiled_cache_size(), 2u);
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionSetPublished));

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, ShaMismatchRetainsActiveSetAndRecovers) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    EvidenceCapture evidence;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer(),
                                                       evidence.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script(kGoodScript);
        const std::string sha = sha256_hex(script);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, index_json(function_json("waf", sha)), "idx-1");
        service.push(script_data_id("waf"), script);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        EXPECT_EQ(store.pin()->edge_function_count(), 1u);
        const auto active = store.pin();

        // New index references different content; the pushed payload hashes
        // to something else.
        const std::string replacement = "$header.x_edge_token == 'other'";
        const std::string replacement_sha = sha256_hex(replacement);
        const std::string stale = "$header.x_edge_token == 'stale'";
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, index_json(function_json("waf", replacement_sha)),
                     "idx-2");
        service.push(script_data_id("waf"), stale);
        co_await yield_updates();
        EXPECT_EQ(store.pin(), active); // all-or-nothing keeps the old set
        EXPECT_EQ(watcher.failed_updates(), 1u);
        expect_failure(watcher.last_failure(), "script", "sha256_mismatch");
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionScriptRejected));
        expect_readiness_value(readiness.current(), false);
        expect_evidence(evidence,
                        {
                                .candidate_status = fiber::access_server::AccessActivationCandidateStatus::Rejected,
                                .failure_code = "sha256_mismatch",
                        });

        // Publishing the payload the index actually references recovers.
        service.push(script_data_id("waf"), replacement);
        co_await wait_for_readiness(readiness, snapshot, true);
        EXPECT_EQ(store.pin()->edge_function_count(), 1u);
        EXPECT_EQ(store.pin()->edge_function_script_bytes(), replacement.size());
        EXPECT_EQ(watcher.successful_updates(), 2u);
        EXPECT_FALSE(watcher.last_failure().has_value());
        expect_evidence(evidence,
                        {
                                .candidate_status = fiber::access_server::AccessActivationCandidateStatus::Accepted,
                                .no_failure = true,
                        });

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, CompileFailureRetainsActiveSet) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script(kGoodScript);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId,
                     index_json(function_json("waf", sha256_hex(script))), "idx-1");
        service.push(script_data_id("waf"), script);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        const auto active = store.pin();
        EXPECT_EQ(active->edge_function_count(), 1u);

        // The payload matches its declared sha but never compiles.
        const std::string bad(kBadScript);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, index_json(function_json("bad", sha256_hex(bad))),
                     "idx-2");
        co_await yield_updates();
        EXPECT_EQ(watcher.script_subscription_count(), 1u); // waf stopped, bad subscribed
        EXPECT_EQ(service.subscribe_attempts(script_data_id("waf")), 1u); // no re-subscribe while dropped
        service.push(script_data_id("bad"), bad);
        co_await yield_updates();
        EXPECT_EQ(store.pin(), active); // the waf set keeps serving
        EXPECT_EQ(watcher.failed_updates(), 1u);
        expect_failure(watcher.last_failure(), "compile", "script_compile_failed");
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionCompileFailed));
        expect_readiness_value(readiness.current(), false);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, ScriptNotFoundRetainsActiveSet) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script(kGoodScript);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId,
                     index_json(function_json("waf", sha256_hex(script))), "idx-1");
        service.push(script_data_id("waf"), script);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        const auto active = store.pin();

        service.push_not_found(script_data_id("waf"));
        co_await yield_updates();
        EXPECT_EQ(store.pin(), active);
        EXPECT_EQ(watcher.failed_updates(), 1u);
        expect_failure(watcher.last_failure(), "script", "script_missing");
        expect_readiness_value(readiness.current(), false);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, NullIndexUnmountsEverything) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    EvidenceCapture evidence;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer(),
                                                       evidence.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script(kGoodScript);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId,
                     index_json(function_json("waf", sha256_hex(script))), "idx-1");
        service.push(script_data_id("waf"), script);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        EXPECT_EQ(store.pin()->edge_function_count(), 1u);

        // JSON null (or an empty functions array) is the explicit unmount.
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, "null", "idx-2");
        co_await yield_updates();
        EXPECT_EQ(store.pin()->edge_function_count(), 0u);
        EXPECT_EQ(store.pin()->edge_function_script_bytes(), 0u);
        EXPECT_EQ(watcher.successful_updates(), 2u);
        EXPECT_EQ(watcher.failed_updates(), 0u);
        EXPECT_EQ(watcher.script_subscription_count(), 0u);
        EXPECT_EQ(service.subscribe_attempts(script_data_id("waf")), 1u);
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionSetRemoved));
        expect_readiness_value(readiness.current(), true); // an empty set is a valid active state
        expect_evidence(evidence, {
                                          .generation = 2u,
                                          .function_count = 0u,
                                          .total_script_bytes = 0u,
                                          .no_failure = true,
                                  });

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, EmptyIndexContentKeepsCurrentSet) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    EvidenceCapture evidence;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer(),
                                                       evidence.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script(kGoodScript);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId,
                     index_json(function_json("waf", sha256_hex(script))), "idx-1");
        service.push(script_data_id("waf"), script);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        const auto active = store.pin();
        EXPECT_EQ(active->edge_function_count(), 1u);

        // Empty content (a NotFound push maps to it) is keep-current, not an
        // unmount: the codec reserves null/[] for that.
        service.push_not_found(fiber::access_server::kEdgeFunctionIndexDataId);
        co_await yield_updates();
        EXPECT_EQ(store.pin(), active);
        EXPECT_EQ(watcher.successful_updates(), 1u);
        EXPECT_EQ(watcher.failed_updates(), 0u);
        EXPECT_EQ(watcher.script_subscription_count(), 1u);
        EXPECT_FALSE(watcher.last_failure().has_value());
        EXPECT_FALSE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionSetRemoved));
        expect_readiness_value(readiness.current(), true);
        expect_evidence(evidence,
                        {
                                .candidate_status = fiber::access_server::AccessActivationCandidateStatus::Accepted,
                                .active_md5 = "idx-1",
                                .function_count = 1u,
                                .no_failure = true,
                        });

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, IndexUpdateStopsRemovedScriptAndReusesCompile) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script_a = "$header.a == '1'";
        const std::string script_b = "$header.b == '2'";
        service.push(
                fiber::access_server::kEdgeFunctionIndexDataId,
                index_json(function_json("a", sha256_hex(script_a)) + "," + function_json("b", sha256_hex(script_b))),
                "idx-1");
        service.push(script_data_id("a"), script_a);
        service.push(script_data_id("b"), script_b);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        EXPECT_EQ(store.pin()->edge_function_count(), 2u);
        EXPECT_EQ(watcher.compiled_cache_size(), 2u);

        // Drop function b: its subscription stops, the set reassembles with
        // one function, and a's program comes from the compile cache.
        const std::string one_function = index_json(function_json("a", sha256_hex(script_a)));
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, one_function, "idx-2");
        co_await yield_updates();
        EXPECT_EQ(watcher.script_subscription_count(), 1u);
        EXPECT_EQ(service.subscribe_attempts(script_data_id("b")), 1u); // dropped, not re-subscribed
        EXPECT_EQ(store.pin()->edge_function_count(), 1u);
        EXPECT_EQ(store.pin()->edge_function_script_bytes(), script_a.size());
        EXPECT_EQ(watcher.successful_updates(), 2u);
        EXPECT_EQ(watcher.compiled_cache_size(), 2u); // b evicts only on cap overflow

        // Republishing identical index bytes is a no-op: a's program comes
        // from the compile cache and no new generation is published.
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, one_function, "idx-3");
        co_await yield_updates();
        EXPECT_EQ(watcher.successful_updates(), 2u);
        EXPECT_EQ(watcher.compiled_cache_size(), 2u);
        EXPECT_EQ(store.pin()->edge_function_count(), 1u);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, SameBytesInTwoPhasesCompileAndCacheSeparately) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        // Identical script bytes mounted twice — once per phase. The body
        // entry must compile (phase joins the cache key) and the head entry
        // stays usable; both programs live in the cache.
        const std::string script = "return true;";
        const std::string two_phases =
                index_json(function_json("head-fn", sha256_hex(script)) + "," +
                           "{\"name\": \"body-fn\", \"sha256\": \"" + sha256_hex(script) +
                           "\", \"phase\": \"request_body\", \"mounts\": [{\"projects\": {\"include\": [\"*\"]}}]}");
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, two_phases, "idx-1");
        service.push(script_data_id("head-fn"), script);
        service.push(script_data_id("body-fn"), script);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        EXPECT_EQ(store.pin()->edge_function_count(), 2u);
        EXPECT_EQ(watcher.compiled_cache_size(), 2u);
        EXPECT_EQ(watcher.successful_updates(), 1u);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, ScriptSubscriptionStartFailureIsReported) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    EvidenceCapture evidence;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer(),
                                                       evidence.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        service.fail_subscriptions(script_data_id("waf"), 1,
                                   fiber::nacos::ConfigServiceError{
                                           .code = fiber::nacos::ConfigServiceErrorCode::Transport,
                                           .io_error = fiber::common::IoErr::NotConnected,
                                           .message = "transient subscription failure",
                                   });

        const std::string script(kGoodScript);
        const std::string index = index_json(function_json("waf", sha256_hex(script)));
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, index, "idx-1");
        co_await yield_updates();
        EXPECT_EQ(watcher.failed_updates(), 1u);
        expect_failure(watcher.last_failure(), "subscription", "script_subscribe_failed");
        if (watcher.last_failure()) {
            EXPECT_NE(watcher.last_failure()->detail.find(script_data_id("waf")), std::string::npos);
        }
        EXPECT_EQ(watcher.script_subscription_count(), 0u); // dead entry dropped
        EXPECT_EQ(store.pin()->edge_function_count(), 0u);
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionScriptSubscriptionFailed));
        expect_evidence(evidence,
                        {.candidate_status = fiber::access_server::AccessActivationCandidateStatus::Rejected});

        // Republishing the index (identical bytes) re-subscribes the dropped
        // Data ID — v1 has no timer retry, the index drives re-delivery.
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, index, "idx-2");
        co_await yield_updates();
        EXPECT_EQ(service.subscribe_attempts(script_data_id("waf")), 2u);
        EXPECT_EQ(watcher.script_subscription_count(), 1u);
        service.push(script_data_id("waf"), script);
        co_await yield_updates();
        EXPECT_EQ(store.pin()->edge_function_count(), 1u);
        EXPECT_EQ(watcher.successful_updates(), 1u);
        expect_evidence(evidence,
                        {
                                .candidate_status = fiber::access_server::AccessActivationCandidateStatus::Accepted,
                                .no_failure = true,
                        });

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, InvalidIndexIsDecodeFailure) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, "{", "idx-1");
        co_await yield_updates();
        EXPECT_EQ(watcher.failed_updates(), 1u);
        expect_failure(watcher.last_failure(), "decode", "invalid_index");
        EXPECT_EQ(store.pin()->edge_function_count(), 0u);
        EXPECT_EQ(watcher.script_subscription_count(), 0u);
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionIndexDecodeFailed));

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, IndexClosedMarksWatcherFailedAndIgnoresLaterPushes) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    MetricsCapture metrics;
    EvidenceCapture evidence;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store, {}, metrics.observer(),
                                                       evidence.observer());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto readiness = watcher.subscribe_readiness();
        EXPECT_TRUE(watcher.start());

        const std::string script(kGoodScript);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId,
                     index_json(function_json("waf", sha256_hex(script))), "idx-1");
        service.push(script_data_id("waf"), script);
        ReadinessWatch::Snapshot snapshot;
        co_await wait_for_readiness(readiness, snapshot, true);
        const auto active = store.pin();

        service.close(fiber::access_server::kEdgeFunctionIndexDataId);
        co_await yield_updates();
        EXPECT_EQ(watcher.state(), fiber::access_server::EdgeFunctionsWatcherState::Failed);
        EXPECT_EQ(watcher.failed_updates(), 1u);
        expect_failure(watcher.last_failure(), "subscription", "index_closed");
        EXPECT_EQ(store.pin(), active); // the serving set survives
        expect_readiness_value(readiness.current(), false);
        EXPECT_TRUE(metrics.saw(fiber::access_server::AccessConfigMetricEvent::EdgeFunctionIndexSubscriptionFailed));
        expect_evidence(evidence, {.watcher_state = "failed"});

        // Pushes after the closure are ignored (state != Running).
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, "null", "idx-2");
        co_await yield_updates();
        EXPECT_EQ(store.pin(), active);
        EXPECT_EQ(watcher.successful_updates(), 1u);

        co_await watcher.shutdown();
        EXPECT_EQ(watcher.state(), fiber::access_server::EdgeFunctionsWatcherState::Stopped);
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(EdgeFunctionsWatcherTest, OversizedScriptIsRejected) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::EdgeFunctionsWatcher watcher(loop, service, store);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        const std::string oversized(
                fiber::access_server::kAccessConfigLimits.edge_functions.max_script_payload_bytes + 1U, 'x');
        const std::string sha = sha256_hex(oversized);
        service.push(fiber::access_server::kEdgeFunctionIndexDataId, index_json(function_json("big", sha)), "idx-1");
        service.push(script_data_id("big"), oversized);
        co_await yield_updates();
        EXPECT_EQ(store.pin()->edge_function_count(), 0u);
        EXPECT_EQ(watcher.failed_updates(), 1u);
        expect_failure(watcher.last_failure(), "script", "script_too_large");

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
