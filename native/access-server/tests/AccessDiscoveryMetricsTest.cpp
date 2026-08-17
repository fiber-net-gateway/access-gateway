#include "observability/AccessDiscoveryMetrics.h"

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <latch>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

std::optional<std::uint64_t> metric_value(std::string_view text, std::string_view prefix) {
    const std::size_t position = text.find(prefix);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t begin = position + prefix.size();
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

TEST(AccessDiscoveryMetricsTest, RendersFixedLifecycleEventsAndResourceAggregates) {
    event::EventLoop loop;
    AccessDiscoveryMetrics metrics(loop);
    const AccessDiscoveryMetricsObserver observer = metrics.observer();
    std::string output;

    async::spawn(loop, [&]() -> async::DetachedTask {
        observer.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Running);
        observer.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Running);
        observer.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Stopped);
        observer.update_transport(AccessNacosTransportComponent::ConfigService,
                                  AccessNacosTransportStatus{
                                          .phase = AccessNacosTransportPhase::ReconnectBackoff,
                                          .failure = AccessNacosTransportFailure::Transport,
                                          .connection_ready_count = 2,
                                          .disconnect_count = 2,
                                          .reconnect_attempt_count = 3,
                                          .subscriptions =
                                                  {
                                                          .active = 4,
                                                          .pending = 2,
                                                          .registered = 2,
                                                          .synchronized = 1,
                                                  },
                                  });
        const AccessDiscoveryStatus reconnecting = metrics.status();
        EXPECT_EQ(reconnecting.lifecycle[static_cast<std::size_t>(AccessNacosComponent::ConfigService)],
                  AccessNacosLifecycleState::Running);
        EXPECT_FALSE(reconnecting.transport[static_cast<std::size_t>(AccessNacosTransportComponent::ConfigService)]
                             .rpc_available);

        observer.update_transport(AccessNacosTransportComponent::ConfigService,
                                  AccessNacosTransportStatus{
                                          .phase = AccessNacosTransportPhase::Ready,
                                          .failure = AccessNacosTransportFailure::None,
                                          .rpc_available = true,
                                          .connection_ready_count = 3,
                                          .disconnect_count = 2,
                                          .reconnect_attempt_count = 3,
                                          .subscriptions =
                                                  {
                                                          .active = 4,
                                                          .registered = 4,
                                                          .synchronized = 4,
                                                  },
                                  });
        observer.update_transport(AccessNacosTransportComponent::NamingService,
                                  AccessNacosTransportStatus{
                                          .phase = AccessNacosTransportPhase::Stopped,
                                          .failure = AccessNacosTransportFailure::Shutdown,
                                          .connection_ready_count = 1,
                                          .disconnect_count = 1,
                                          .subscriptions =
                                                  {
                                                          .active = 5,
                                                          .pending = 5,
                                                  },
                                          .registrations =
                                                  {
                                                          .pending = 1,
                                                  },
                                  });

        const AccessDiscoveryServiceAggregate first{
                .ready = true,
                .selectable_endpoints = 3,
                .logical_clusters = 2,
        };
        const AccessDiscoveryServiceAggregate second{
                .ready = true,
                .selectable_endpoints = 2,
                .logical_clusters = 1,
        };
        const AccessDiscoveryServiceAggregate changed{
                .ready = true,
                .selectable_endpoints = 1,
                .logical_clusters = 1,
        };
        observer.transition_service(AccessDiscoveryMetricEvent::ServiceUpdateChanged, {}, first);
        observer.transition_service(AccessDiscoveryMetricEvent::ServiceUpdateChanged, {}, second);
        observer.transition_service(AccessDiscoveryMetricEvent::ServiceUpdateChanged, first, changed);
        observer.record_event(AccessDiscoveryMetricEvent::ServiceUpdateUnchanged);
        observer.transition_service(AccessDiscoveryMetricEvent::ServiceRetiredSubscriptionClosed, second, {});
        observer.record_event(AccessDiscoveryMetricEvent::SelectorAcquireTransport);
        observer.record_event(AccessDiscoveryMetricEvent::SelectorAcquireSucceeded);
        observer.record_event(AccessDiscoveryMetricEvent::SelectorAcquireSucceeded);
        observer.selector_lease(true);
        observer.selector_lease(true);
        observer.selector_lease(false);

        metrics.append_prometheus(output);
        loop.stop();
        co_return;
    });
    loop.run();
    observer.selector_lease(false);

    EXPECT_NE(output.find("access_server_nacos_component_lifecycle{component=\"client\",state=\"running\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_component_lifecycle{component=\"client\",state=\"created\"} 0"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_component_lifecycle{component=\"config_service\",state=\"running\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_component_lifecycle{component=\"naming_service\",state=\"stopped\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_transport_phase{component=\"config_service\",phase=\"ready\"} 1"),
              std::string::npos);
    EXPECT_NE(
            output.find(
                    "access_server_nacos_transport_phase{component=\"config_service\",phase=\"reconnect_backoff\"} 0"),
            std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_transport_failure{component=\"config_service\",category=\"none\"} 1"),
              std::string::npos);
    EXPECT_NE(
            output.find("access_server_nacos_transport_failure{component=\"config_service\",category=\"transport\"} 0"),
            std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_rpc_available{component=\"config_service\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_rpc_available{component=\"naming_service\"} 0"), std::string::npos);
    EXPECT_NE(
            output.find("access_server_nacos_connection_events_total{component=\"config_service\",event=\"ready\"} 3"),
            std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_connection_events_total{component=\"config_service\",event=\"reconnect_"
                          "attempt\"} 3"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_subscriptions{component=\"config_service\",state=\"synchronized\"} 4"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_nacos_registrations{state=\"pending\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_events_total{operation=\"update\",result=\"success\",reason="
                          "\"changed\"} 3"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_events_total{operation=\"update\",result=\"ignored\",reason="
                          "\"unchanged\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_events_total{operation=\"retire\",result=\"retired\",reason="
                          "\"subscription_closed\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_events_total{operation=\"acquire\",result=\"failure\",reason="
                          "\"transport\"} 1"),
              std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_resources{resource=\"ready_service\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_resources{resource=\"selectable_endpoint\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_resources{resource=\"logical_cluster\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_discovery_resources{resource=\"selector_lease\"} 1"), std::string::npos);
    EXPECT_EQ(output.find("orders-secret"), std::string::npos);
}

TEST(AccessDiscoveryMetricsTest, ReadersNeverObserveTornStatusAggregates) {
    event::EventLoop loop;
    AccessDiscoveryMetrics metrics(loop);
    const AccessDiscoveryMetricsObserver observer = metrics.observer();
    std::atomic<bool> done = false;
    std::atomic<bool> inconsistent = false;
    std::atomic<std::uint64_t> reads = 0;
    std::latch reader_started(1);

    std::thread reader([&]() {
        bool first = true;
        do {
            std::string output;
            metrics.append_prometheus(output);
            const auto endpoints =
                    metric_value(output, "access_server_discovery_resources{resource=\"selectable_endpoint\"} ");
            const auto clusters =
                    metric_value(output, "access_server_discovery_resources{resource=\"logical_cluster\"} ");
            const auto ready = metric_value(
                    output,
                    "access_server_nacos_connection_events_total{component=\"config_service\",event=\"ready\"} ");
            const auto disconnected = metric_value(
                    output,
                    "access_server_nacos_connection_events_total{component=\"config_service\",event=\"disconnect\"} ");
            const auto synchronized = metric_value(
                    output, "access_server_nacos_subscriptions{component=\"config_service\",state=\"synchronized\"} ");
            if (first) {
                first = false;
                reader_started.count_down();
            }
            if (!endpoints || !clusters || *endpoints != *clusters || !ready || !disconnected || !synchronized ||
                *ready != *disconnected || *ready != *synchronized) {
                inconsistent.store(true, std::memory_order_release);
                break;
            }
            reads.fetch_add(1, std::memory_order_relaxed);
        } while (!done.load(std::memory_order_acquire));
    });
    reader_started.wait();

    async::spawn(loop, [&]() -> async::DetachedTask {
        AccessDiscoveryServiceAggregate current{
                .ready = true,
        };
        observer.transition_service(AccessDiscoveryMetricEvent::ServiceUpdateChanged, {}, current);
        for (std::uint64_t value = 1; value <= 10000; ++value) {
            observer.update_transport(AccessNacosTransportComponent::ConfigService,
                                      AccessNacosTransportStatus{
                                              .phase = AccessNacosTransportPhase::Ready,
                                              .rpc_available = true,
                                              .connection_ready_count = value,
                                              .disconnect_count = value,
                                              .subscriptions =
                                                      {
                                                              .synchronized = value,
                                                      },
                                      });
            AccessDiscoveryServiceAggregate next{
                    .ready = true,
                    .selectable_endpoints = value,
                    .logical_clusters = value,
            };
            observer.transition_service(AccessDiscoveryMetricEvent::ServiceUpdateChanged, current, next);
            current = next;
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
