#include "observability/AccessDiscoveryMetrics.h"
#include "runtime/NacosStatusMonitor.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {
namespace {

using namespace std::chrono_literals;

struct InteropResult {
    bool initial_ready = false;
    bool initial_query = false;
    bool fault_observed = false;
    bool recovered = false;
    bool recovered_query = false;
    bool stopped = false;
    std::uint64_t reconnect_attempts = 0;
};

std::optional<std::uint16_t> parse_port(std::string_view text) noexcept {
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 || value > 65535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

template<typename Predicate>
async::Task<bool> wait_until(Predicate predicate, std::chrono::milliseconds timeout) noexcept {
    const auto deadline = event::EventLoop::current().now() + timeout;
    while (!predicate()) {
        if (event::EventLoop::current().now() >= deadline) {
            co_return false;
        }
        co_await async::sleep(10ms);
    }
    co_return true;
}

std::pair<const AccessNacosTransportStatus &, const AccessNacosTransportStatus &>
transport_pair(const AccessDiscoveryStatus &status) noexcept {
    return {
            status.transport[static_cast<std::size_t>(AccessNacosTransportComponent::ConfigService)],
            status.transport[static_cast<std::size_t>(AccessNacosTransportComponent::NamingService)],
    };
}

bool both_ready(const AccessDiscoveryStatus &status) noexcept {
    const auto [config, naming] = transport_pair(status);
    return config.phase == AccessNacosTransportPhase::Ready && config.rpc_available &&
           naming.phase == AccessNacosTransportPhase::Ready && naming.rpc_available;
}

async::DetachedTask run_interop(event::EventLoop *loop, nacos::NacosClientConfig client_config,
                                std::shared_ptr<std::promise<InteropResult>> finished,
                                std::shared_ptr<std::atomic<bool>> ready_for_fault) noexcept {
    InteropResult result;
    nacos::NacosClientOptions client_options;
    client_options.connect_timeout = 1s;
    client_options.request_timeout = 2s;
    client_options.retry_initial_delay = 20ms;
    client_options.retry_max_delay = 100ms;
    auto client_created = nacos::NacosClient::create(*loop, std::move(client_config), client_options);
    if (!client_created) {
        finished->set_value(result);
        co_return;
    }
    auto client = std::move(*client_created);

    nacos::ConfigServiceOptions config_options;
    config_options.rpc.connect_timeout = 500ms;
    config_options.rpc.request_timeout = 2s;
    config_options.rpc.handshake_timeout = 2s;
    config_options.rpc.compatibility_setup_delay = 50ms;
    config_options.rpc.heartbeat_interval = 250ms;
    config_options.rpc.reconnect_initial_delay = 20ms;
    config_options.rpc.reconnect_max_delay = 100ms;
    auto config_created = nacos::ConfigService::create(*client, config_options);
    if (!config_created) {
        finished->set_value(result);
        co_return;
    }
    auto config_service = std::move(*config_created);

    nacos::NamingServiceOptions naming_options;
    naming_options.rpc = config_options.rpc;
    auto naming_created = nacos::NamingService::create(*client, naming_options);
    if (!naming_created) {
        finished->set_value(result);
        co_return;
    }
    auto naming_service = std::move(*naming_created);

    AccessDiscoveryMetrics metrics(*loop);
    NacosStatusMonitor monitor(*loop, *config_service, *naming_service, metrics.observer());
    monitor.start();

    const bool client_started = client->start().has_value();
    bool config_attempted = false;
    bool naming_attempted = false;
    if (client_started) {
        config_attempted = true;
        if (config_service->start()) {
            naming_attempted = true;
            if (naming_service->start()) {
                result.initial_ready = co_await wait_until([&metrics]() { return both_ready(metrics.status()); }, 15s);
            }
        }
    }

    std::uint64_t config_ready_count = 0;
    std::uint64_t naming_ready_count = 0;
    std::uint64_t config_disconnect_count = 0;
    std::uint64_t naming_disconnect_count = 0;
    if (result.initial_ready) {
        const auto [config, naming] = transport_pair(metrics.status());
        config_ready_count = config.connection_ready_count;
        naming_ready_count = naming.connection_ready_count;
        config_disconnect_count = config.disconnect_count;
        naming_disconnect_count = naming.disconnect_count;
        auto queried = co_await config_service->get_config("access-server-external-interop-probe", "DEFAULT_GROUP");
        result.initial_query = queried.has_value();
        ready_for_fault->store(true, std::memory_order_release);

        result.fault_observed = co_await wait_until(
                [&metrics, config_disconnect_count, naming_disconnect_count]() {
                    const auto [config, naming] = transport_pair(metrics.status());
                    return config.disconnect_count > config_disconnect_count &&
                           naming.disconnect_count > naming_disconnect_count;
                },
                10s);
        if (result.fault_observed) {
            result.recovered = co_await wait_until(
                    [&metrics, config_ready_count, naming_ready_count]() {
                        const auto status = metrics.status();
                        const auto [config, naming] = transport_pair(status);
                        return both_ready(status) && config.connection_ready_count > config_ready_count &&
                               naming.connection_ready_count > naming_ready_count;
                    },
                    15s);
        }
        if (result.recovered) {
            auto queried = co_await config_service->get_config("access-server-external-interop-probe", "DEFAULT_GROUP");
            result.recovered_query = queried.has_value();
        }
    }

    if (naming_attempted) {
        co_await naming_service->shutdown();
    }
    if (config_attempted) {
        co_await config_service->shutdown();
    }
    co_await monitor.shutdown();
    const auto [final_config, final_naming] = transport_pair(metrics.status());
    result.stopped = (!config_attempted || final_config.phase == AccessNacosTransportPhase::Stopped) &&
                     (!naming_attempted || final_naming.phase == AccessNacosTransportPhase::Stopped) &&
                     !final_config.rpc_available && !final_naming.rpc_available;
    result.reconnect_attempts = final_config.reconnect_attempt_count + final_naming.reconnect_attempt_count;
    co_await client->shutdown();
    finished->set_value(result);
}

} // namespace
} // namespace fiber::access_server

int main(int argc, char **argv) {
    using namespace std::chrono_literals;
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <rnacos-http-port> <fault-proxy-grpc-port>\n", argv[0]);
        return 2;
    }
    const auto http_port = fiber::access_server::parse_port(argv[1]);
    const auto grpc_port = fiber::access_server::parse_port(argv[2]);
    if (!http_port || !grpc_port) {
        std::fprintf(stderr, "interop ports must be in the range 1..65535\n");
        return 2;
    }

    fiber::nacos::NacosClientConfigParams params;
    params.server_hosts.push_back("127.0.0.1");
    params.http_port = *http_port;
    params.grpc_port = *grpc_port;
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    if (!config) {
        std::fprintf(stderr, "failed to create external rnacos client configuration\n");
        return 1;
    }

    fiber::event::EventLoopGroup group(1);
    group.start();
    auto finished = std::make_shared<std::promise<fiber::access_server::InteropResult>>();
    auto future = finished->get_future();
    auto ready_for_fault = std::make_shared<std::atomic<bool>>(false);
    fiber::async::spawn(
            group.at(0), [loop = &group.at(0), config = std::move(*config), finished, ready_for_fault]() mutable {
                return fiber::access_server::run_interop(loop, std::move(config), finished, ready_for_fault);
            });

    bool marker_written = false;
    const auto deadline = std::chrono::steady_clock::now() + 45s;
    while (future.wait_for(10ms) != std::future_status::ready && std::chrono::steady_clock::now() < deadline) {
        if (!marker_written && ready_for_fault->load(std::memory_order_acquire)) {
            std::fputs("ACCESS_SERVER_INTEROP_READY_FOR_DROP\n", stdout);
            std::fflush(stdout);
            marker_written = true;
        }
    }
    if (future.wait_for(0ms) != std::future_status::ready) {
        std::fprintf(stderr, "external rnacos interoperability timed out\n");
        group.stop();
        group.join();
        return 1;
    }
    const fiber::access_server::InteropResult result = future.get();
    group.stop();
    group.join();

    const bool success = marker_written && result.initial_ready && result.initial_query && result.fault_observed &&
                         result.recovered && result.recovered_query && result.stopped && result.reconnect_attempts > 0;
    if (!success) {
        std::fprintf(stderr,
                     "external rnacos interoperability failed: marker=%d initial_ready=%d initial_query=%d "
                     "fault=%d recovered=%d recovered_query=%d stopped=%d reconnect_attempts=%llu\n",
                     marker_written, result.initial_ready, result.initial_query, result.fault_observed,
                     result.recovered, result.recovered_query, result.stopped,
                     static_cast<unsigned long long>(result.reconnect_attempts));
        return 1;
    }
    std::printf("external rnacos reconnect interoperability passed (reconnect_attempts=%llu)\n",
                static_cast<unsigned long long>(result.reconnect_attempts));
    return 0;
}
