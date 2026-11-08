#include "../src/observability/AccessConfigMetrics.h"

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

#include "../src/config/AccessConfig.h"
#include "../src/routing/EdgeFunctionLibrary.h"
#include "../src/routing/EdgeFunctionsSnapshot.h"
#include "../src/runtime/RouteConfigStore.h"

namespace fiber::access_server {
namespace {

ProjectConfig metrics_project_config() {
    ProjectConfig config;
    config.version = 7;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = "secret-host.example.com",
                    .strategy = HostStrategyConfig{},
            },
    };
    RouteConfig route;
    route.path = "/secret-route";
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Text,
            .content = "response",
    };
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    return config;
}

std::optional<std::uint64_t> metric_value(std::string_view text, std::string_view series) {
    std::size_t position = 0;
    for (;;) {
        position = text.find(series, position);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        if (position == 0 || text[position - 1] == '\n') {
            break;
        }
        position += series.size();
    }
    const std::size_t begin = position + series.size();
    const std::size_t end = text.find('\n', begin);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto converted = std::from_chars(text.data() + begin, text.data() + end, value);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + end) {
        return std::nullopt;
    }
    return value;
}

TEST(AccessConfigMetricsTest, RendersFixedEventsReadinessAndSnapshotAggregates) {
    event::EventLoop loop;
    AccessConfigMetrics metrics(loop);
    const AccessConfigMetricsObserver observer = metrics.observer();
    RouteConfigStore store;
    auto published = store.apply("secret-project", metrics_project_config());
    ASSERT_TRUE(published);
    std::string output;

    async::spawn(loop, [&]() -> async::DetachedTask {
        observer.on_event(observer.context, AccessConfigMetricEvent::ProjectRoutePublished);
        observer.on_event(observer.context, AccessConfigMetricEvent::ProjectRoutePublished);
        observer.on_event(observer.context, AccessConfigMetricEvent::ProjectListDecodeFailed);
        observer.on_duration(observer.context, AccessConfigMetricStage::ProjectCompile, std::chrono::nanoseconds(7));
        observer.on_duration(observer.context, AccessConfigMetricStage::ProjectCompile, std::chrono::nanoseconds(5));
        observer.on_duration(observer.context, AccessConfigMetricStage::ServiceReady, std::chrono::nanoseconds(11));
        observer.on_readiness(observer.context, AccessConfigMetricReadiness{
                                                        .state = AccessConfigMetricReadinessState::Ready,
                                                        .desired_projects = 3,
                                                        .subscribed_projects = 3,
                                                        .synchronized_projects = 3,
                                                        .rejected_projects = 1,
                                                });
        observer.on_snapshot(observer.context, *store.pin());
        metrics.append_prometheus(output, event::EventLoop::current().now() + std::chrono::milliseconds(2500));
        loop.stop();
        co_return;
    });
    loop.run();

    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"project_route\",result=\"success\",reason="
                          "\"published\"} 2"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"project_list\",result=\"failure\",reason="
                          "\"decode\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_stage_duration_nanoseconds_total{stage=\"project_compile\"} 12"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_stage_duration_observations_total{stage=\"project_compile\"} 2"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_stage_duration_max_nanoseconds{stage=\"project_compile\"} 7"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_stage_duration_nanoseconds_total{stage=\"service_ready\"} 11"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_readiness{state=\"ready\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_config_readiness{state=\"unavailable\"} 0"), std::string::npos);
    EXPECT_NE(output.find("access_server_config_projects{state=\"desired\"} 3"), std::string::npos);
    EXPECT_NE(output.find("access_server_config_projects{state=\"ready_to_publish\"} 0"), std::string::npos);
    EXPECT_NE(output.find("access_server_config_projects{state=\"rejected\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_route_snapshot_resources{resource=\"project\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_route_snapshot_resources{resource=\"host\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_route_snapshot_resources{resource=\"route\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_route_snapshot_generation 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_route_snapshot_age_seconds 2"), std::string::npos);
    const auto estimated_bytes = metric_value(output, "access_server_route_snapshot_estimated_bytes ");
    ASSERT_TRUE(estimated_bytes);
    EXPECT_GT(*estimated_bytes, 0u);
    EXPECT_EQ(output.find("secret-project"), std::string::npos);
    EXPECT_EQ(output.find("secret-host.example.com"), std::string::npos);
    EXPECT_EQ(output.find("/secret-route"), std::string::npos);
}

TEST(AccessConfigMetricsTest, RendersEdgeFunctionEventsAndSnapshotGauges) {
    event::EventLoop loop;
    AccessConfigMetrics metrics(loop);
    const AccessConfigMetricsObserver observer = metrics.observer();
    RouteConfigStore store;

    EdgeFunctionLibrary library;
    auto program = library.compile("$header.a == '1'");
    ASSERT_TRUE(program.has_value());
    auto state = std::make_shared<EdgeFunctionsState>();
    state->index.version = 1;
    state->index.functions = std::vector<EdgeFunctionIndexEntry>{
            EdgeFunctionIndexEntry{
                    .name = "waf",
                    .sha256_hex = std::string(64, '0'),
                    .projects = EdgeFunctionProjectsSelector{.include = {"*"}},
            },
    };
    state->programs = std::vector<std::shared_ptr<EdgeFunctionProgram>>{
            std::make_shared<EdgeFunctionProgram>(std::move(*program))};
    state->total_script_bytes = 17;
    store.set_edge_functions(std::move(state));
    std::string output;

    async::spawn(loop, [&]() -> async::DetachedTask {
        observer.on_event(observer.context, AccessConfigMetricEvent::EdgeFunctionSetPublished);
        observer.on_event(observer.context, AccessConfigMetricEvent::EdgeFunctionSetRemoved);
        observer.on_event(observer.context, AccessConfigMetricEvent::EdgeFunctionIndexSubscriptionFailed);
        observer.on_event(observer.context, AccessConfigMetricEvent::EdgeFunctionIndexDecodeFailed);
        observer.on_event(observer.context, AccessConfigMetricEvent::EdgeFunctionScriptSubscriptionFailed);
        observer.on_event(observer.context, AccessConfigMetricEvent::EdgeFunctionScriptRejected);
        observer.on_event(observer.context, AccessConfigMetricEvent::EdgeFunctionCompileFailed);
        observer.on_snapshot(observer.context, *store.pin());
        metrics.append_prometheus(output, event::EventLoop::current().now());
        loop.stop();
        co_return;
    });
    loop.run();

    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"edge_function_set\",result=\"success\","
                          "reason=\"published\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"edge_function_set\",result=\"success\","
                          "reason=\"removed\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"edge_function_index\",result=\"failure\","
                          "reason=\"subscription\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"edge_function_index\",result=\"failure\","
                          "reason=\"decode\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"edge_function_script\",result=\"failure\","
                          "reason=\"subscription\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"edge_function_script\",result=\"failure\","
                          "reason=\"rejected\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_config_updates_total{resource=\"edge_function_script\",result=\"failure\","
                          "reason=\"compile\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_route_snapshot_resources{resource=\"edge_function\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_route_snapshot_edge_function_script_bytes 17"), std::string::npos);
}

TEST(AccessConfigMetricsTest, ReadersNeverObserveTornReadinessSamples) {
    event::EventLoop loop;
    AccessConfigMetrics metrics(loop);
    const AccessConfigMetricsObserver observer = metrics.observer();
    std::atomic<bool> done = false;
    std::atomic<bool> inconsistent = false;
    std::atomic<std::uint64_t> reads = 0;
    std::latch reader_started(1);

    std::thread reader([&]() {
        bool first = true;
        do {
            std::string output;
            metrics.append_prometheus(output, std::chrono::steady_clock::now());
            const auto desired = metric_value(output, "access_server_config_projects{state=\"desired\"} ");
            const auto subscribed = metric_value(output, "access_server_config_projects{state=\"subscribed\"} ");
            const auto synchronized = metric_value(output, "access_server_config_projects{state=\"synchronized\"} ");
            const bool invalid =
                    !desired || !subscribed || !synchronized || *desired != *subscribed || *desired != *synchronized;
            if (first) {
                first = false;
                reader_started.count_down();
            }
            if (invalid) {
                inconsistent.store(true, std::memory_order_release);
                break;
            }
            reads.fetch_add(1, std::memory_order_relaxed);
        } while (!done.load(std::memory_order_acquire));
    });
    reader_started.wait();

    async::spawn(loop, [&]() -> async::DetachedTask {
        for (std::uint64_t value = 1; value <= 10000; ++value) {
            observer.on_readiness(observer.context,
                                  AccessConfigMetricReadiness{
                                          .state = AccessConfigMetricReadinessState::SynchronizingProjects,
                                          .desired_projects = value,
                                          .subscribed_projects = value,
                                          .synchronized_projects = value,
                                  });
        }
        done.store(true, std::memory_order_release);
        loop.stop();
        co_return;
    });
    loop.run();
    reader.join();

    EXPECT_FALSE(inconsistent.load(std::memory_order_acquire));
    EXPECT_GT(reads.load(std::memory_order_relaxed), 0u);
}

} // namespace
} // namespace fiber::access_server
