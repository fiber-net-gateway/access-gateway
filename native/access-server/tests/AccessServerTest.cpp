#include "../src/runtime/AccessServer.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClient.h>
#include <fiber/cat/CatClientConfig.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

#include "../src/observability/AccessConfigMetrics.h"

namespace fiber::access_server {
namespace {

using namespace std::chrono_literals;

std::uint16_t listener_port(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

std::string request(std::uint16_t port, std::string_view extra_headers = {}, std::string_view method = "GET",
                    std::string_view target = "/") {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }

    std::string payload(method);
    payload.push_back(' ');
    payload.append(target);
    payload.append(" HTTP/1.1\r\nHost: api.example.com\r\n");
    payload.append(extra_headers);
    payload.append("Connection: close\r\n\r\n");
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t size = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
        if (size <= 0) {
            ::close(fd);
            return {};
        }
        sent += static_cast<std::size_t>(size);
    }

    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t size = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (size == 0) {
            break;
        }
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            response.clear();
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(size));
    }
    ::close(fd);
    return response;
}

std::optional<std::string> gunzip_response_body(std::string_view response) {
    const std::size_t body_start = response.find("\r\n\r\n");
    if (body_start == std::string_view::npos) {
        return std::nullopt;
    }
    response.remove_prefix(body_start + 4);

    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        return std::nullopt;
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(response.data()));
    stream.avail_in = static_cast<uInt>(response.size());
    std::array<unsigned char, 4096> buffer{};
    std::string output;
    int result = Z_OK;
    do {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            (void) inflateEnd(&stream);
            return std::nullopt;
        }
        output.append(reinterpret_cast<const char *>(buffer.data()), buffer.size() - stream.avail_out);
    } while (result != Z_STREAM_END);
    if (inflateEnd(&stream) != Z_OK) {
        return std::nullopt;
    }
    return output;
}

ProjectConfig response_config() {
    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = "api.example.com",
                    .strategy = HostStrategyConfig{},
            },
    };
    RouteConfig route;
    route.path = "/";
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Text,
            .content = "ok",
    };
    route.gzip = ResponseGzipConfig{.enabled = true};
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    return config;
}

TEST(AccessDnsServiceTest, ShutdownDoesNotBlockTheCallingEventLoop) {
    event::EventLoop control_loop;
    event::EventLoopGroup workers(1);
    AccessDnsService dns;
    std::promise<bool> initialized_promise;
    auto initialized = initialized_promise.get_future();
    std::promise<void> marker_promise;
    auto marker = marker_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    std::atomic<bool> marker_seen = false;
    bool marker_ready_before_worker_start = false;
    bool marker_seen_before_shutdown_completed = false;

    async::spawn(control_loop, [&]() -> async::DetachedTask {
        const bool init_ok = co_await dns.init(workers);
        initialized_promise.set_value(init_ok);
        if (!init_ok) {
            stopped_promise.set_value();
            control_loop.stop();
            co_return;
        }

        async::spawn([&]() -> async::DetachedTask {
            marker_seen.store(true, std::memory_order_release);
            marker_promise.set_value();
            co_return;
        });
        co_await dns.shutdown();
        co_await dns.shutdown();
        marker_seen_before_shutdown_completed = marker_seen.load(std::memory_order_acquire);
        stopped_promise.set_value();
        control_loop.stop();
    });

    std::thread worker_starter([&]() {
        marker_ready_before_worker_start = marker.wait_for(2s) == std::future_status::ready;
        workers.start();
    });

    control_loop.run();
    worker_starter.join();
    workers.stop();
    workers.join();

    ASSERT_EQ(initialized.wait_for(0s), std::future_status::ready);
    EXPECT_TRUE(initialized.get());
    EXPECT_EQ(stopped.wait_for(0s), std::future_status::ready);
    EXPECT_TRUE(marker_ready_before_worker_start);
    EXPECT_TRUE(marker_seen_before_shutdown_completed);
}

TEST(AccessServerTest, ServesPublishedSnapshotAndShutsDownWorkerResources) {
    RouteConfigStore store;
    auto published = store.apply("demo", response_config());
    ASSERT_TRUE(published);

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    AccessConfigMetrics config_metrics(accept_loop);
    AccessServer server(accept_loop, workers, store, {},
                        AccessServerOptions{
                                .access_log = AccessLogOptions{.query_hash_enabled = true},
                                .config_metrics = &config_metrics,
                        });
    std::promise<std::pair<std::uint16_t, std::uint16_t>> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    testing::internal::CaptureStderr();
    workers.start();
    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        const AccessConfigMetricsObserver config_observer = config_metrics.observer();
        config_observer.on_event(config_observer.context, AccessConfigMetricEvent::ProjectRoutePublished);
        config_observer.on_readiness(config_observer.context, AccessConfigMetricReadiness{
                                                                      .state = AccessConfigMetricReadinessState::Ready,
                                                                      .desired_projects = 1,
                                                                      .subscribed_projects = 1,
                                                                      .synchronized_projects = 1,
                                                              });
        config_observer.on_snapshot(config_observer.context, *store.pin());
        auto initialized = co_await server.initialize();
        if (!initialized) {
            port_promise.set_value({0, 0});
            accept_loop.stop();
            co_return;
        }
        auto loopback = net::IpAddress::v4({127, 0, 0, 1});
        auto bound = server.bind(net::SocketAddress(loopback, 0));
        if (!bound) {
            port_promise.set_value({0, 0});
            co_await server.shutdown_and_wait();
            accept_loop.stop();
            co_return;
        }
        auto metrics_bound = server.bind_metrics(net::SocketAddress(loopback, 0));
        if (!metrics_bound) {
            port_promise.set_value({0, 0});
            co_await server.shutdown_and_wait();
            accept_loop.stop();
            co_return;
        }
        startup_ok = true;
        port_promise.set_value({listener_port(server.fd()), listener_port(server.metrics_fd())});
        async::spawn([&server]() { return server.serve(); });
        async::spawn([&server]() { return server.serve_metrics(); });
    });

    std::string response;
    std::string gzip_response;
    std::string gzip_head_response;
    std::string unacceptable_response;
    std::string metrics_response;
    std::thread client([&]() {
        const auto [bound_port, metrics_port] = port.get();
        if (bound_port != 0 && metrics_port != 0) {
            response = request(bound_port,
                               "X-Real-Ip: 203.0.113.77\r\n"
                               "X-Forwarded-Proto: https\r\n",
                               "GET", "/?token=integration-secret");
            gzip_response = request(bound_port, "Accept-Encoding: gzip\r\n");
            gzip_head_response = request(bound_port, "Accept-Encoding: gzip\r\n", "HEAD");
            unacceptable_response = request(bound_port, "Accept-Encoding: gzip;q=0, identity;q=0\r\n");
            metrics_response = request(metrics_port);
        }
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            if (startup_ok) {
                co_await server.shutdown_and_wait();
            }
            stopped_promise.set_value();
            accept_loop.stop();
        });
    });

    accept_loop.run();
    client.join();
    EXPECT_EQ(stopped.wait_for(2s), std::future_status::ready);
    workers.stop();
    workers.join();
    const std::string access_logs = testing::internal::GetCapturedStderr();

    ASSERT_TRUE(startup_ok);
    EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(response.find("Vary: Accept-Encoding"), std::string::npos);
    EXPECT_EQ(response.find("Content-Encoding: gzip"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\nok"));
    EXPECT_NE(gzip_response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(gzip_response.find("Content-Encoding: gzip"), std::string::npos);
    EXPECT_EQ(gunzip_response_body(gzip_response), "ok");
    EXPECT_NE(gzip_head_response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(gzip_head_response.find("Content-Encoding: gzip"), std::string::npos);
    EXPECT_TRUE(gzip_head_response.ends_with("\r\n\r\n"));
    EXPECT_NE(unacceptable_response.find("HTTP/1.1 406"), std::string::npos);
    EXPECT_NE(unacceptable_response.find("Vary: Accept-Encoding"), std::string::npos);
    EXPECT_EQ(unacceptable_response.find("Content-Encoding: gzip"), std::string::npos);
    EXPECT_NE(metrics_response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_requests_total{result=\"success\"} 3"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_requests_total{result=\"client_error\"} 1"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_request_duration_seconds_count 4"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_response_compression_total{result=\"gzip\"} 2"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_response_compression_total{result=\"identity\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_response_compression_total{result=\"not_acceptable\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_config_updates_total{resource=\"project_route\",result=\"success\","
                                    "reason=\"published\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_config_readiness{state=\"ready\"} 1"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_route_snapshot_resources{resource=\"project\"} 1"),
              std::string::npos);
    EXPECT_EQ(access_logs.find("integration-secret"), std::string::npos);
    EXPECT_NE(access_logs.find("path=\"/\" query=\"\""), std::string::npos);
    EXPECT_NE(access_logs.find("query_hash=\"hmac-sha256:"), std::string::npos);
    EXPECT_NE(access_logs.find("query_filtered=true"), std::string::npos);
    EXPECT_EQ(access_logs.find("203.0.113.77"), std::string::npos);
    EXPECT_NE(access_logs.find("client_ip=\"127.0.0.1\" peer_ip=\"127.0.0.1\""), std::string::npos);
    EXPECT_NE(access_logs.find("forwarding_status=ignored_direct_mode"), std::string::npos);
}

TEST(AccessServerTest, ReturnsCatTraceIdFromTheUnifiedRequestContext) {
    RouteConfigStore store;
    auto published = store.apply("demo", response_config());
    ASSERT_TRUE(published);

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    event::EventLoopGroup cat_group(1);
    cat::CatClientConfigParams cat_params{
            .app_key = "unified-access-server",
            .hostname = "access-test",
            .ip = "127.0.0.1",
            .thread_group_name = "access-test-cat",
            .thread_id = "0",
            .thread_name = "cat-sender",
            .bootstrap_collectors =
                    {
                            net::SocketAddress(net::IpAddress::v4({127, 0, 0, 1}), 1),
                    },
    };
    auto cat_config = cat::CatClientConfig::create(std::move(cat_params));
    ASSERT_TRUE(cat_config);
    cat::CatClientOptions cat_options;
    cat_options.enable_heartbeat = false;
    cat_options.enable_system_stats = false;
    cat_options.shutdown_drain_timeout = 10ms;
    auto cat_client = cat::CatClient::create(cat_group.at(0), std::move(*cat_config), cat_options);
    ASSERT_TRUE(cat_client);

    AccessServer server(accept_loop, workers, store, {},
                        AccessServerOptions{
                                .cat_client = cat_client->get(),
                        });
    std::promise<bool> cat_started_promise;
    auto cat_started = cat_started_promise.get_future();
    std::promise<std::uint16_t> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    workers.start();
    cat_group.start();
    async::spawn(cat_group.at(0), [&]() -> async::DetachedTask {
        cat_started_promise.set_value((*cat_client)->start().has_value());
        co_return;
    });
    ASSERT_EQ(cat_started.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(cat_started.get());

    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        auto initialized = co_await server.initialize();
        if (!initialized) {
            port_promise.set_value(0);
            accept_loop.stop();
            co_return;
        }
        auto loopback = net::IpAddress::v4({127, 0, 0, 1});
        auto bound = server.bind(net::SocketAddress(loopback, 0));
        if (!bound) {
            port_promise.set_value(0);
            co_await server.shutdown_and_wait();
            accept_loop.stop();
            co_return;
        }
        startup_ok = true;
        port_promise.set_value(listener_port(server.fd()));
        async::spawn([&server]() { return server.serve(); });
    });

    std::string response;
    std::thread client([&]() {
        const std::uint16_t bound_port = port.get();
        if (bound_port != 0) {
            response = request(bound_port);
        }
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            if (startup_ok) {
                co_await server.shutdown_and_wait();
            }
            stopped_promise.set_value();
            accept_loop.stop();
        });
    });

    accept_loop.run();
    client.join();
    EXPECT_EQ(stopped.wait_for(2s), std::future_status::ready);

    std::promise<void> cat_stopped_promise;
    auto cat_stopped = cat_stopped_promise.get_future();
    async::spawn(cat_group.at(0), [&]() -> async::DetachedTask {
        co_await (*cat_client)->shutdown();
        cat_stopped_promise.set_value();
    });
    ASSERT_EQ(cat_stopped.wait_for(2s), std::future_status::ready);
    workers.stop();
    cat_group.stop();
    workers.join();
    cat_group.join();

    ASSERT_TRUE(startup_ok);
    const std::size_t trace = response.find("Hi-Trace-Id: ");
    ASSERT_NE(trace, std::string::npos);
    const std::size_t trace_end = response.find("\r\n", trace);
    ASSERT_NE(trace_end, std::string::npos);
    EXPECT_GT(trace_end, trace + std::string_view("Hi-Trace-Id: ").size());
}

} // namespace
} // namespace fiber::access_server
