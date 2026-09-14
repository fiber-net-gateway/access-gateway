#include "../src/runtime/AccessServer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClient.h>
#include <fiber/cat/CatClientConfig.h>
#include <fiber/common/util/Base64.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/HttpClientTlsOptions.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsPemSource.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <openssl/sha.h>

#include "../src/observability/AccessRuntimeMetrics.h"
#include "../src/runtime/AccessDnsService.h"
#include "support/ZlibReference.h"

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

    const fiber::test::ZlibReferenceResult result = fiber::test::zlib_reference_gunzip(response);
    if (!result.ok || !result.stream_end) {
        return std::nullopt;
    }
    return std::move(result.output);
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

struct FailingDnsResolverFactory {
    AccessDnsResolverFactory delegate = AccessDnsResolverFactory::system();
    std::size_t create_calls = 0;
    std::size_t fail_on_call = 0;

    [[nodiscard]] AccessDnsResolverFactory adapter() noexcept {
        return AccessDnsResolverFactory{
                .context = this,
                .create = create,
        };
    }

    [[nodiscard]] static bool create(void *context, event::EventLoop &loop, dns::SharedDnsCache2 &cache,
                                     const dns::DnsClient::Options &client_options,
                                     std::unique_ptr<dns::DnsResolverLocal> &local,
                                     std::unique_ptr<dns::DnsResolver> &resolver) noexcept {
        auto &self = *static_cast<FailingDnsResolverFactory *>(context);
        ++self.create_calls;
        const bool created = self.delegate.create(self.delegate.context, loop, cache, client_options, local, resolver);
        return created && self.create_calls != self.fail_on_call;
    }
};

struct ObservingDnsResolverFactory {
    AccessDnsResolverFactory delegate = AccessDnsResolverFactory::system();
    std::size_t create_calls = 0;
    std::size_t nameserver_count = 0;
    std::array<net::SocketAddress, dns::kMaxDnsNameservers> nameservers{};
    std::chrono::milliseconds timeout{0};
    std::uint8_t attempts = 0;
    bool rotate = false;

    [[nodiscard]] AccessDnsResolverFactory adapter() noexcept {
        return AccessDnsResolverFactory{
                .context = this,
                .create = create,
        };
    }

    [[nodiscard]] static bool create(void *context, event::EventLoop &loop, dns::SharedDnsCache2 &cache,
                                     const dns::DnsClient::Options &client_options,
                                     std::unique_ptr<dns::DnsResolverLocal> &local,
                                     std::unique_ptr<dns::DnsResolver> &resolver) noexcept {
        auto &self = *static_cast<ObservingDnsResolverFactory *>(context);
        ++self.create_calls;
        self.nameserver_count = client_options.nameservers.size();
        for (std::size_t i = 0; i < self.nameserver_count; ++i) {
            self.nameservers[i] = client_options.nameservers[i];
        }
        self.timeout = client_options.timeout;
        self.attempts = client_options.attempts;
        self.rotate = client_options.rotate_nameservers;
        return self.delegate.create(self.delegate.context, loop, cache, client_options, local, resolver);
    }
};

TEST(AccessDnsServiceTest, KeepsResolverFactoryAsTwoPointerValueAdapter) {
    EXPECT_TRUE(std::is_trivially_copyable_v<AccessDnsResolverFactory>);
    EXPECT_LE(sizeof(AccessDnsResolverFactory), 2U * sizeof(void *));
}

TEST(AccessDnsServiceTest, ReleasesPartialInitializationAndCanRetry) {
    event::EventLoop control_loop;
    event::EventLoopGroup workers(2);
    FailingDnsResolverFactory factory{.fail_on_call = 2};
    AccessDnsService dns(factory.adapter());
    bool first_initialized = true;
    bool second_initialized = false;

    workers.start();
    async::spawn(control_loop, [&]() -> async::DetachedTask {
        first_initialized = co_await dns.init(workers);
        EXPECT_FALSE(first_initialized);
        EXPECT_EQ(factory.create_calls, 2U);

        factory.fail_on_call = 0;
        second_initialized = co_await dns.init(workers);
        EXPECT_TRUE(second_initialized);
        EXPECT_EQ(factory.create_calls, 4U);

        co_await dns.shutdown();
        co_await dns.shutdown();
        control_loop.stop();
    });
    control_loop.run();
    workers.stop();
    workers.join();

    EXPECT_FALSE(first_initialized);
    EXPECT_TRUE(second_initialized);
}

TEST(AccessDnsServiceTest, InjectsCompleteValidatedOptionsIntoEveryWorker) {
    AccessDnsMetrics metrics;
    AccessDnsServiceOptions options;
    ASSERT_TRUE(options.client.nameservers.add(net::SocketAddress(net::IpAddress::v4({192, 0, 2, 1}), 53)));
    ASSERT_TRUE(options.client.nameservers.add(
            net::SocketAddress(net::IpAddress::v6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), 53)));
    ASSERT_TRUE(options.client.nameservers.add(net::SocketAddress(net::IpAddress::v4({192, 0, 2, 2}), 53)));
    options.client.timeout = 3s;
    options.client.attempts = 4;
    options.client.rotate_nameservers = true;
    options.source = AccessDnsConfigSource::System;
    options.unsupported = dns::ResolverUnsupportedFeature::Search;
    options.metrics = metrics.observer();

    event::EventLoop control_loop;
    event::EventLoopGroup workers(2);
    ObservingDnsResolverFactory factory;
    AccessDnsService service(std::move(options), factory.adapter());
    workers.start();
    async::spawn(control_loop, [&]() -> async::DetachedTask {
        EXPECT_TRUE(co_await service.init(workers));
        co_await service.shutdown();
        control_loop.stop();
    });
    control_loop.run();
    workers.stop();
    workers.join();

    EXPECT_EQ(factory.create_calls, 2U);
    EXPECT_EQ(factory.nameserver_count, 3U);
    EXPECT_EQ(factory.nameservers[0].to_string(), "192.0.2.1:53");
    EXPECT_EQ(factory.nameservers[1].to_string(), "[2001:db8::1]:53");
    EXPECT_EQ(factory.nameservers[2].to_string(), "192.0.2.2:53");
    EXPECT_EQ(factory.timeout, 3s);
    EXPECT_EQ(factory.attempts, 4U);
    EXPECT_TRUE(factory.rotate);
    const AccessDnsMetricsStatus status = metrics.status();
    EXPECT_EQ(status.configured_nameservers, 3U);
    EXPECT_EQ(status.initialization_successes, 1U);
    EXPECT_EQ(status.initialization_failures, 0U);
    EXPECT_EQ(status.state, AccessDnsResolverState::Stopped);
    EXPECT_EQ(status.active_resolvers, 0U);
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
    AccessRuntimeMetrics runtime_metrics(accept_loop);
    AccessActivationEvidenceStore activation_evidence(accept_loop, AccessActivationEvidenceIdentity{
                                                                           .instance_id = "access-test-0",
                                                                           .build_version = "test",
                                                                           .build_revision = "test-revision",
                                                                           .started_at_unix_millis = 1000,
                                                                   });
    AccessServer server(accept_loop, workers, store, {},
                        AccessServerOptions{
                                .access_log = AccessLogOptions{.query_hash_enabled = true},
                                .runtime_metrics = &runtime_metrics,
                                .activation_evidence = &activation_evidence,
                                .activation_endpoint =
                                        AccessActivationEndpointOptions{
                                                .enabled = true,
                                                .instance_id = "access-test-0",
                                                .bearer_token = "0123456789abcdef0123456789abcdef",
                                        },
                        });
    std::promise<std::pair<std::uint16_t, std::uint16_t>> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    testing::internal::CaptureStderr();
    workers.start();
    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        AccessRouteActivationEvidence route_evidence;
        route_evidence.watcher_state = "running";
        route_evidence.readiness_state = "ready";
        route_evidence.snapshot_generation = 1;
        route_evidence.projects.push_back(AccessActivationProjectEvidence{.name = "a"});
        route_evidence.projects.push_back(AccessActivationProjectEvidence{.name = "b"});
        activation_evidence.route_observer().on_update(activation_evidence.route_observer().context, route_evidence);
        const AccessConfigMetricsObserver config_observer = runtime_metrics.config().observer();
        config_observer.on_event(config_observer.context, AccessConfigMetricEvent::ProjectRoutePublished);
        config_observer.on_readiness(config_observer.context, AccessConfigMetricReadiness{
                                                                      .state = AccessConfigMetricReadinessState::Ready,
                                                                      .desired_projects = 1,
                                                                      .subscribed_projects = 1,
                                                                      .synchronized_projects = 1,
                                                              });
        config_observer.on_snapshot(config_observer.context, *store.pin());
        const AccessDiscoveryMetricsObserver discovery_observer = runtime_metrics.discovery().observer();
        discovery_observer.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Running);
        discovery_observer.transition_service(AccessDiscoveryMetricEvent::ServiceUpdateChanged, {},
                                              AccessDiscoveryServiceAggregate{
                                                      .ready = true,
                                                      .selectable_endpoints = 2,
                                                      .logical_clusters = 1,
                                              });
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
    std::string unauthorized_activation_response;
    std::string activation_response;
    std::string changed_activation_response;
    std::string invalid_activation_response;
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
            metrics_response = request(metrics_port, {}, "GET", "/metrics");
            unauthorized_activation_response = request(metrics_port, {}, "GET", "/v1/activation-evidence?limit=1");
            activation_response = request(metrics_port, "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n",
                                          "GET", "/v1/activation-evidence?limit=1");
            changed_activation_response =
                    request(metrics_port, "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n", "GET",
                            "/v1/activation-evidence?cursor=1%3A1&limit=1");
            invalid_activation_response =
                    request(metrics_port, "Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n", "GET",
                            "/v1/activation-evidence?limit=257");
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
    EXPECT_NE(metrics_response.find("access_server_response_compression_total{"
                                    "result=\"not_acceptable\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_config_updates_total{resource="
                                    "\"project_route\",result=\"success\","
                                    "reason=\"published\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_config_readiness{state=\"ready\"} 1"), std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_route_snapshot_resources{resource=\"project\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_nacos_component_lifecycle{"
                                    "component=\"client\",state=\"running\"} "
                                    "1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_discovery_resources{resource=\"ready_service\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics_response.find("access_server_discovery_resources{resource="
                                    "\"selectable_endpoint\"} 2"),
              std::string::npos);
    EXPECT_NE(unauthorized_activation_response.find("HTTP/1.1 401"), std::string::npos);
    EXPECT_EQ(unauthorized_activation_response.find("0123456789abcdef"), std::string::npos);
    EXPECT_NE(activation_response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(activation_response.find("\"contractVersion\":1"), std::string::npos);
    EXPECT_NE(activation_response.find("\"id\":\"access-test-0\""), std::string::npos);
    EXPECT_NE(activation_response.find("\"publicationMode\":\"atomic_request_pin\""), std::string::npos);
    EXPECT_NE(activation_response.find("\"nextCursor\":\"2:1\""), std::string::npos);
    EXPECT_NE(activation_response.find("Cache-Control: no-store"), std::string::npos);
    EXPECT_EQ(activation_response.find("0123456789abcdef"), std::string::npos);
    EXPECT_NE(changed_activation_response.find("HTTP/1.1 409"), std::string::npos);
    EXPECT_NE(changed_activation_response.find("evidence_changed"), std::string::npos);
    EXPECT_NE(invalid_activation_response.find("HTTP/1.1 400"), std::string::npos);
    EXPECT_NE(invalid_activation_response.find("invalid_page"), std::string::npos);
    EXPECT_EQ(access_logs.find("integration-secret"), std::string::npos);
    EXPECT_NE(access_logs.find("path=\"/\" query=\"\""), std::string::npos);
    EXPECT_NE(access_logs.find("query_hash=\"hmac-sha256:"), std::string::npos);
    EXPECT_NE(access_logs.find("query_filtered=true"), std::string::npos);
    EXPECT_EQ(access_logs.find("203.0.113.77"), std::string::npos);
    EXPECT_NE(access_logs.find("client_ip=\"127.0.0.1\" peer_ip=\"127.0.0.1\""), std::string::npos);
    EXPECT_NE(access_logs.find("forwarding_status=ignored_direct_mode"), std::string::npos);
}

// Self-signed localhost pair (same fixture material as the fiber endpoint
// tests); test credentials only.
const char kWsCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUEDCdxH6aX38+fEeFx3nlY3pJwdkwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDExNzEzMDcwNVoXDTI3MDEx
NzEzMDcwNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA4+tN+7EU3WmwFfjE4bn720reQJkTnAOUOYXg9zejQ75q
vHOpFxLU9z866mVpT7jVYAupmKfXrJ9U5Vd9znrWFzZt9rTdg+hISdujXjaEfEf+
GQ+66xthO2tAF3c6XokoqRpJR0GVInJoWaHBpV0PcvRb9AhRfuk+ja3W1dfdHnE8
LWutJCVK0HOWifIBGqpED3YMBNKZxFSKTCKLiqbxmnd6TT1fh8UI+AibEKhuJX4A
m3enMonO1PHeSOUY1dfXpZfdRdnYgjiyVyEw7oQL11r6O2LJZMJsoW912uIUnYrs
A4bDbMMfDgHe+PiyERCG62xydAlj1phGVlbGI/8HOQIDAQABo1MwUTAdBgNVHQ4E
FgQUvM4+Ad+L+GYd6i4nZgRFaPkRo7UwHwYDVR0jBBgwFoAUvM4+Ad+L+GYd6i4n
ZgRFaPkRo7UwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAxo8i
jbyceTsjxiMDoXd/OPtPCD2CcpWOUxMb4hdGk3pMK6xFq8c7bdMcn6oZMF7xpdHg
jDTrfa8TlPITcG/34MtvPS3hq7klCPi948Z9wbtJWGfKAl3rHYK7PIIj3wNipTcQ
IkfIlO/t6VKPSx1S9HQA6nCDOvCufOL54Mfz0vI9Y47c4O1TNtbJiiWUkP/pEjEw
RMeULfoobqmMYTjbjQ8nKC25cQAmhQ0koOqJPquPtAHvaowqBT6jDLEL+8vR4Kfc
9UqEtfRr0+7LgbcofOsseDFYMPBW2GdpPMJ2PMYsQtFMXRoomlhjdpIct6e3rRnd
GiDzEZ0VwkYlJDwF4w==
-----END CERTIFICATE-----
)";

const char kWwKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDj6037sRTdabAV
+MThufvbSt5AmROcA5Q5heD3N6NDvmq8c6kXEtT3PzrqZWlPuNVgC6mYp9esn1Tl
V33OetYXNm32tN2D6EhJ26NeNoR8R/4ZD7rrG2E7a0AXdzpeiSipGklHQZUicmhZ
ocGlXQ9y9Fv0CFF+6T6NrdbV190ecTwta60kJUrQc5aJ8gEaqkQPdgwE0pnEVIpM
IouKpvGad3pNPV+HxQj4CJsQqG4lfgCbd6cyic7U8d5I5RjV19ell91F2diCOLJX
ITDuhAvXWvo7Yslkwmyhb3Xa4hSdiuwDhsNswx8OAd74+LIREIbrbHJ0CWPWmEZW
VsYj/wc5AgMBAAECggEAHomvmDKg1g3MHxWG46u0uCwu3T7lZrkACjkK7HTS9ke0
K23f0Qyf5kTdkvxlgN4GEOlfHuoWNrXefSAc5iaFOvT7BNw09fCQhvzbxcrOM4y9
2gPGiqvPelOjccFy26nK/eVcviRmZAgqPSA0PwDaCg/9phPbP4Lm87rAF0TmBqbq
n5s+7MXf4iFTbRIec2zTikWfbUglhNmKr3eC/4+K+hk3TX95Wltvz6dGz+godV/L
FilwLEa+e0cSTUA8FYzYtoEUiV7/8dl8VBIvQWtx8sRNNihCmnlYrJ3N8tw/hO6F
PKpfoOo+L9uRJG4LGtAkM0Pqs9U9uN5v7F5HNMxO1QKBgQD61LhiF/ftPlTRFQm2
CrnIN4PcQtIDRar/cuwgyq3F8AAfJ5PSYD/GvitaQYxa9Ya1IM3T7UPx6L3OmJl6
updR3Mh/+6BtAYwSwoWLv0tHQ01xOe9pwML52JShVocVXQFE/UXNtuffuUpXVeWk
miVen8SI4CHLeFU+6Dfcp0l3owKBgQDonbYbB9bRVzG0gbgdp2K1pxvMQizR8IkU
GsYaT/LMooBpRBOHrane+9KCztkghjmTyDKEl7jwt65fvFl0ttkipq1ISTepV6Rt
Cmdc5PnBc+ON49/6ivTGFAdU5CY3sE/7L6ngPqZq6bq8nBJ0NPcjpfEl2JfBeND8
NisrSQEjcwKBgQDlcp1QLji/LtuLf0Eo41rbCd13KTDPiXVIw6m4vW6EuGyEE0In
mZ/9f4xMvdVUh3C4U8+04z/aFFs8l18eY310hxBp8pXn4RhvOL3M/iowgCJhRuv4
wzoYLsSXaX2cTz2QDFdEPOKTRv34Mj0le1Rf4Kp5wv1nESZ5qxceo3CTHQKBgFWb
jSR/ixB57YIH53GKY6qEuJdAl2wgAOLUQ6n1WF71Qxr6gdGCGS1GMiAP7hqpK1F2
8RiZGegFQXhcQfPRQzIcc1NSFtkMtyemF4o5fq0ycEGM5qY3M4QeZOBaIrKGAblo
vjUX+XkJUb8OFUCNKZMGBCywfJEoXIklilegw3l/AoGBALtmVrX28WQ42DOYWdKD
dmDMBg1+21d8wIWs4k5bu1LdlY8XqMnV9TAHwOwGcleK2uM3AfoLOho6HwFwdyhJ
x20XBogOziImjh+cvWNpm951EC3oWHOFYPsMjX1mRCye88LQHwm3gQ8iCIOzPj+8
RB6SahiCZEhAtLq/9Q/O1bL5
-----END PRIVATE KEY-----
)";

std::string websocket_accept(std::string_view key) {
    std::string source(key);
    source.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    std::array<std::uint8_t, SHA_DIGEST_LENGTH> digest{};
    if (SHA1(reinterpret_cast<const std::uint8_t *>(source.data()), source.size(), digest.data()) == nullptr) {
        return {};
    }
    return util::base64_encode(digest.data(), digest.size());
}

// Minimal blocking HTTP/1.1 WebSocket upstream: one connection, one upgraded
// session, raw frames both ways.
class WebSocketUpstreamFixture {
public:
    WebSocketUpstreamFixture() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listen_fd_, 1) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        port_ = ntohs(bound.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~WebSocketUpstreamFixture() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] const std::string &observed_request() const noexcept { return observed_request_; }
    [[nodiscard]] const std::string &client_data() const noexcept { return client_data_; }

private:
    void serve() {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        timeval timeout{.tv_sec = 2, .tv_usec = 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        std::string request_text;
        std::array<char, 4096> buffer{};
        while (request_text.find("\r\n\r\n") == std::string::npos) {
            const ssize_t size = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (size <= 0) {
                ::close(fd);
                return;
            }
            request_text.append(buffer.data(), static_cast<std::size_t>(size));
        }
        observed_request_ = request_text;

        std::string key;
        std::string lowercase_request = request_text;
        std::transform(lowercase_request.begin(), lowercase_request.end(), lowercase_request.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        constexpr std::string_view kKeyPrefix = "sec-websocket-key:";
        const std::size_t key_at = lowercase_request.find(kKeyPrefix);
        if (key_at != std::string::npos) {
            std::size_t begin = key_at + kKeyPrefix.size();
            while (begin < request_text.size() && (request_text[begin] == ' ' || request_text[begin] == '\t')) {
                ++begin;
            }
            const std::size_t end = request_text.find("\r\n", begin);
            if (end != std::string::npos) {
                key = request_text.substr(begin, end - begin);
            }
        }

        std::string response("HTTP/1.1 101 Switching Protocols\r\n"
                             "Connection: Upgrade\r\n"
                             "Upgrade: websocket\r\n"
                             "Sec-WebSocket-Accept: ");
        response.append(websocket_accept(key));
        response.append("\r\n\r\nserver-frame");
        std::size_t sent = 0;
        while (sent < response.size()) {
            const ssize_t size = ::send(fd, response.data() + sent, response.size() - sent, 0);
            if (size <= 0) {
                ::close(fd);
                return;
            }
            sent += static_cast<std::size_t>(size);
        }

        client_data_.reserve(64);
        while (client_data_.size() < 12) {
            const ssize_t size = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (size <= 0) {
                break;
            }
            client_data_.append(buffer.data(), static_cast<std::size_t>(size));
        }
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::string observed_request_;
    std::string client_data_;
    std::thread thread_{};
};

struct H2WebSocketClientOutcome {
    common::IoErr error = common::IoErr::None;
    bool extended_connect_enabled = false;
    int status_code = 0;
    std::string accept;
    std::string connection;
    std::string upgrade;
    std::string body;
};

async::DetachedTask run_h2_websocket_client(event::EventLoop &loop, std::uint16_t port,
                                            std::promise<H2WebSocketClientOutcome> *promise) {
    H2WebSocketClientOutcome outcome;
    http::HttpClientTlsOptions tls;
    tls.server_name = "localhost";

    auto connection = std::make_shared<http::Http2ClientConnection>(loop);
    auto connected = co_await connection->connect(net::SocketAddress(net::IpAddress::loopback_v4(), port), 5s, tls);
    if (!connected) {
        outcome.error = connected.error();
        promise->set_value(std::move(outcome));
        co_return;
    }

    struct RunState {
        std::atomic_bool done{false};
    };
    auto run_state = std::make_shared<RunState>();
    async::spawn(loop, [connection, run_state]() -> async::DetachedTask {
        (void) co_await connection->wait_closed();
        run_state->done.store(true, std::memory_order_release);
    });

    mem::BufPool pool;
    http::ClientHttp2Exchange exchange(*connection, pool);
    for (int i = 0; i < 500; ++i) {
        if (exchange.extended_connect_support() == http::Http2ExtendedConnectSupport::Enabled) {
            outcome.extended_connect_enabled = true;
            break;
        }
        co_await async::sleep(1ms);
    }
    if (!outcome.extended_connect_enabled) {
        outcome.error = common::IoErr::NotSupported;
    } else {
        http::HttpHeaders headers(pool);
        headers.set("Sec-WebSocket-Version", "13");
        auto sent = co_await exchange.send_request_header(
                {
                        .method = http::HttpMethod::Connect,
                        .scheme = "https",
                        .authority = "api.example.com",
                        .path = "/ws",
                        .protocol = "websocket",
                        .headers = &headers,
                },
                false, 2s);
        if (!sent) {
            outcome.error = sent.error();
        } else {
            auto header = co_await exchange.read_header(2s);
            if (!header) {
                outcome.error = header.error();
            } else {
                outcome.status_code = (*header)->status_code;
                outcome.accept.assign((*header)->headers.get("sec-websocket-accept"));
                outcome.connection.assign((*header)->headers.get("connection"));
                outcome.upgrade.assign((*header)->headers.get("upgrade"));

                static constexpr std::string_view kClientFrame = "client-frame";
                auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kClientFrame.data()),
                                                           kClientFrame.size(), false, 2s);
                if (!written) {
                    outcome.error = written.error();
                } else {
                    while (outcome.body.size() < std::string_view("server-frame").size()) {
                        auto body = co_await exchange.read_body(64, 2s);
                        if (!body) {
                            outcome.error = body.error();
                            break;
                        }
                        const bool complete = body->complete();
                        while (mem::IoBuf *part = body->first_readable()) {
                            outcome.body.append(reinterpret_cast<const char *>(part->readable_data()),
                                                part->readable());
                            body->consume_and_compact(part->readable());
                        }
                        if (complete) {
                            break;
                        }
                    }
                }
            }
        }
    }

    if (exchange.valid()) {
        (void) exchange.abort();
    }
    connection->shutdown();
    for (int i = 0; i < 500 && !run_state->done.load(std::memory_order_acquire); ++i) {
        co_await async::sleep(1ms);
    }
    promise->set_value(std::move(outcome));
}

TEST(AccessServerTest, ServesWebSocketExtendedConnectOverHttp2Tls) {
    WebSocketUpstreamFixture upstream;
    ASSERT_NE(upstream.port(), 0);

    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = "api.example.com",
                    .strategy = HostStrategyConfig{},
            },
    };
    RouteConfig route;
    route.path = "/ws";
    route.addresses = {std::optional<std::string>("127.0.0.1:" + std::to_string(upstream.port()))};
    route.timeout_millis = 2000;
    route.websocket_timeout_millis = 1000;
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};

    RouteConfigStore store;
    auto published = store.apply("orders", std::move(config));
    ASSERT_TRUE(published) << published.error().message;

    net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = net::TlsPemSource::from_content(kWsCertPem);
    credential_options.private_key = net::TlsPemSource::from_content(kWwKeyPem);
    auto credential = net::TlsCredential::create(credential_options);
    ASSERT_TRUE(credential);
    http::HttpServerTlsOptions tls{};
    tls.configure_callback = &net::configure_tls_with_credential;
    tls.configure_ctx = credential->get();

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    AccessServer server(accept_loop, workers, store, {},
                        AccessServerOptions{
                                .http_server = {.tls = tls},
                        });
    std::promise<std::uint16_t> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    workers.start();
    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        auto initialized = co_await server.initialize();
        if (!initialized) {
            port_promise.set_value(0);
            accept_loop.stop();
            co_return;
        }
        auto bound = server.bind(net::SocketAddress(net::IpAddress::v4({127, 0, 0, 1}), 0));
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

    H2WebSocketClientOutcome client_outcome;
    std::thread client([&]() {
        const std::uint16_t bound_port = port.get();
        if (bound_port != 0) {
            std::promise<H2WebSocketClientOutcome> client_done;
            auto client_future = client_done.get_future();
            async::spawn(accept_loop, [&]() { return run_h2_websocket_client(accept_loop, bound_port, &client_done); });
            client_outcome = client_future.get();
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
    ASSERT_TRUE(startup_ok);
    EXPECT_EQ(stopped.wait_for(2s), std::future_status::ready);
    workers.stop();
    workers.join();

    // The TLS h2 listener must advertise SETTINGS_ENABLE_CONNECT_PROTOCOL
    // (RFC 8441) and translate the extended CONNECT into an upstream upgrade.
    EXPECT_EQ(client_outcome.error, common::IoErr::None);
    EXPECT_TRUE(client_outcome.extended_connect_enabled);
    EXPECT_EQ(client_outcome.status_code, 200);
    EXPECT_TRUE(client_outcome.accept.empty());
    EXPECT_TRUE(client_outcome.connection.empty());
    EXPECT_TRUE(client_outcome.upgrade.empty());
    EXPECT_EQ(client_outcome.body, "server-frame");

    EXPECT_NE(upstream.observed_request().find("GET /ws HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(upstream.observed_request().find("Upgrade: websocket"), std::string::npos);
    const std::size_t key_at = upstream.observed_request().find("Sec-WebSocket-Key: ");
    ASSERT_NE(key_at, std::string::npos);
    EXPECT_EQ(upstream.observed_request().substr(key_at + 18, 24).find("\r"), std::string::npos);
    EXPECT_NE(upstream.observed_request().find("Sec-WebSocket-Version: 13"), std::string::npos);
    EXPECT_EQ(upstream.client_data(), "client-frame");
}

TEST(AccessServerTest, InjectsNetworkEntryBeforeHostPolicyAndReplacesClientSuppliedEntry) {
    ProjectConfig config = response_config();
    ASSERT_TRUE(config.hosts.has_value() && !config.hosts->empty());
    config.hosts->front().strategy = HostStrategyConfig{.net_mask = kNetVdi};
    RouteConfigStore store;
    auto published = store.apply("demo", std::move(config));
    ASSERT_TRUE(published);

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    AccessServer server(accept_loop, workers, store, {},
                        AccessServerOptions{
                                .network_entry = "vdi",
                        });
    std::promise<std::uint16_t> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    testing::internal::CaptureStderr();
    workers.start();
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

    std::string injected_response;
    std::string spoofed_response;
    std::thread client([&]() {
        const std::uint16_t bound_port = port.get();
        if (bound_port != 0) {
            // No X-Entry from the client: the injected deployment entry must
            // satisfy the VDI-only host policy...
            injected_response = request(bound_port);
            // ...and a client-supplied entry must never override it.
            spoofed_response = request(bound_port, "X-Entry: desktop\r\n");
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
    (void) testing::internal::GetCapturedStderr();

    ASSERT_TRUE(startup_ok);
    EXPECT_NE(injected_response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_TRUE(injected_response.ends_with("\r\n\r\nok"));
    EXPECT_NE(spoofed_response.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_TRUE(spoofed_response.ends_with("\r\n\r\nok"));
}

// The entry gate is server-authoritative even when the deployment declares
// no entry network: a client-supplied X-Entry is stripped before host policy,
// so an entry-gated host rejects the request instead of honoring the forged
// value.
TEST(AccessServerTest, EmptyNetworkEntryStripsClientSuppliedEntryBeforeHostPolicy) {
    ProjectConfig config = response_config();
    ASSERT_TRUE(config.hosts.has_value() && !config.hosts->empty());
    config.hosts->front().strategy = HostStrategyConfig{.net_mask = kNetVdi};
    RouteConfigStore store;
    auto published = store.apply("demo", std::move(config));
    ASSERT_TRUE(published);

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    AccessServer server(accept_loop, workers, store, {}, AccessServerOptions{});
    std::promise<std::uint16_t> port_promise;
    auto port = port_promise.get_future();
    std::promise<void> stopped_promise;
    auto stopped = stopped_promise.get_future();
    bool startup_ok = false;

    testing::internal::CaptureStderr();
    workers.start();
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

    std::string forged_response;
    std::thread client([&]() {
        const std::uint16_t bound_port = port.get();
        if (bound_port != 0) {
            forged_response = request(bound_port, "X-Entry: vdi\r\n");
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
    (void) testing::internal::GetCapturedStderr();

    ASSERT_TRUE(startup_ok);
    EXPECT_NE(forged_response.find("HTTP/1.1 403"), std::string::npos);
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
