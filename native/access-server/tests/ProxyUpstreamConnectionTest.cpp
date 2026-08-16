#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TlsContext.h>
#include "QuicTestTlsCertificate.h"
#include "execution/ProxyUpstreamConnection.h"

namespace {

using namespace std::chrono_literals;

const fiber::access_server::UpstreamTlsClientPolicy kLegacyUpstreamTls;

std::optional<std::uint16_t> bound_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::nullopt;
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, address)) {
        return std::nullopt;
    }
    return address.port();
}

struct ResolverState {
    std::vector<fiber::net::IpAddress> addresses;
    std::size_t calls = 0;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

fiber::async::Task<fiber::common::IoResult<std::vector<fiber::net::IpAddress>>>
resolve_addresses(void *context, std::string_view) noexcept {
    auto &state = *static_cast<ResolverState *>(context);
    ++state.calls;
    if (state.error != fiber::common::IoErr::None) {
        co_return std::unexpected(state.error);
    }
    co_return state.addresses;
}

fiber::access_server::ProxyDnsResolver resolver_adapter(ResolverState &state) noexcept {
    return {
            .context = &state,
            .resolve = resolve_addresses,
    };
}

struct ConnectionScenarioResult {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::access_server::ProxyConnectErrorCode error_code = fiber::access_server::ProxyConnectErrorCode::Connect;
    fiber::net::IpAddress connected_ip;
    std::size_t resolver_calls = 0;
    std::size_t worker_index = 0;
    std::size_t connection_loop_index = 0;
    bool first_hit = false;
    bool second_hit = false;
    bool tls_enabled = false;
    bool verify_peer = false;
    std::string ca_file;
    std::string server_name;
    std::string verify_name;
    fiber::access_server::ProxyConnectionObservation observation;
    fiber::access_server::ProxyConnectionObservation second_observation;
};

fiber::async::DetachedTask run_tls_server(fiber::net::TcpListener *listener, fiber::net::TlsContext *context,
                                          std::promise<fiber::common::IoErr> *promise) {
    auto accepted = co_await listener->accept();
    listener->close();
    if (!accepted) {
        promise->set_value(accepted.error());
        co_return;
    }
    auto created =
            fiber::http::TlsTransport::create(fiber::event::EventLoop::current(), std::move(*accepted), *context);
    if (!created) {
        promise->set_value(created.error());
        co_return;
    }
    auto handshake = co_await (*created)->handshake(2s);
    (*created)->close();
    promise->set_value(handshake ? fiber::common::IoErr::None : handshake.error());
}

fiber::async::DetachedTask run_tls_client_scenario(fiber::http::StealableHttp1ConnectionPoolSet *pool,
                                                   fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                                   fiber::access_server::UpstreamTlsClientPolicy policy,
                                                   std::promise<ConnectionScenarioResult> *promise) {
    ConnectionScenarioResult result;
    auto connected = co_await fiber::access_server::acquire_proxy_upstream_connection(
            *pool, resolver_adapter(*resolver), key, policy, 500ms);
    result.resolver_calls = resolver->calls;
    if (!connected) {
        result.error = connected.error().io_error;
        result.error_code = connected.error().code;
        result.observation = connected.error().observation;
    } else {
        result.observation = connected->observation;
        const auto &tls = connected->connection->options().tls;
        result.first_hit = connected->lease.hit();
        result.connected_ip = connected->connection->options().peer_addr.ip();
        result.tls_enabled = tls.enabled;
        result.verify_peer = tls.verify_peer;
        result.ca_file = tls.ca_file;
        result.server_name = tls.server_name;
        result.verify_name = tls.verify_name;
        connected->lease.reset();
    }
    co_await pool->shutdown_async();
    promise->set_value(std::move(result));
}

ConnectionScenarioResult run_tls_scenario(const std::string &certificate_path, const std::string &private_key_path,
                                          std::string_view host, fiber::access_server::UpstreamTlsClientPolicy policy,
                                          bool use_ip_key = false) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    if (!pool.init()) {
        return ConnectionScenarioResult{.error = fiber::common::IoErr::NoMem};
    }
    fiber::net::TcpListener listener(group.at(0));
    auto bound = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bound) {
        return ConnectionScenarioResult{.error = bound.error()};
    }
    auto port = bound_port(listener.fd());
    if (!port) {
        listener.close();
        return ConnectionScenarioResult{.error = fiber::common::IoErr::Invalid};
    }

    fiber::net::TlsOptions server_options;
    server_options.cert_file = certificate_path;
    server_options.key_file = private_key_path;
    fiber::net::TlsContext server_context(std::move(server_options), true);
    auto initialized = server_context.init();
    if (!initialized) {
        listener.close();
        return ConnectionScenarioResult{.error = initialized.error()};
    }
    auto key =
            use_ip_key
                    ? fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), *port,
                                                                    fiber::http::Http1ConnectionGroupKey::Scheme::Https)
                    : fiber::http::Http1ConnectionGroupKey::from_name(
                              host, *port, fiber::http::Http1ConnectionGroupKey::Scheme::Https);
    if (!key) {
        listener.close();
        return ConnectionScenarioResult{.error = fiber::common::IoErr::Invalid};
    }
    ResolverState resolver{
            .addresses = {fiber::net::IpAddress::loopback_v4()},
    };
    std::promise<fiber::common::IoErr> server_promise;
    auto server_future = server_promise.get_future();
    std::promise<ConnectionScenarioResult> client_promise;
    auto client_future = client_promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_tls_server(&listener, &server_context, &server_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return run_tls_client_scenario(&pool, *key, &resolver, std::move(policy), &client_promise);
    });

    ConnectionScenarioResult result = client_future.get();
    (void) server_future.get();
    group.stop();
    group.join();
    return result;
}

fiber::async::DetachedTask run_ip_scenario(fiber::http::StealableHttp1ConnectionPoolSet *pool,
                                           fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                           std::promise<ConnectionScenarioResult> *promise) {
    ConnectionScenarioResult result;
    const fiber::access_server::ProxyDnsResolver dns_resolver =
            resolver ? resolver_adapter(*resolver) : fiber::access_server::ProxyDnsResolver{};
    auto connected = co_await fiber::access_server::acquire_proxy_upstream_connection(*pool, dns_resolver, key,
                                                                                      kLegacyUpstreamTls, 500ms);
    result.resolver_calls = resolver ? resolver->calls : 0;
    if (!connected) {
        result.error = connected.error().io_error;
        result.error_code = connected.error().code;
        result.observation = connected.error().observation;
    } else {
        result.observation = connected->observation;
        result.first_hit = connected->lease.hit();
        result.connected_ip = connected->connection->options().peer_addr.ip();
        connected->lease.reset();
    }
    co_await pool->shutdown_async();
    promise->set_value(std::move(result));
}

fiber::async::DetachedTask run_pool_hit_scenario(fiber::http::StealableHttp1ConnectionPoolSet *pool,
                                                 fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                                 std::promise<ConnectionScenarioResult> *promise) {
    ConnectionScenarioResult result;
    auto first = co_await fiber::access_server::acquire_proxy_upstream_connection(*pool, resolver_adapter(*resolver),
                                                                                  key, kLegacyUpstreamTls, 500ms);
    if (!first) {
        result.error = first.error().io_error;
        result.error_code = first.error().code;
        result.observation = first.error().observation;
        result.resolver_calls = resolver->calls;
        co_await pool->shutdown_async();
        promise->set_value(std::move(result));
        co_return;
    }
    result.observation = first->observation;
    result.first_hit = first->lease.hit();
    first->lease.reset();

    auto second = co_await fiber::access_server::acquire_proxy_upstream_connection(*pool, resolver_adapter(*resolver),
                                                                                   key, kLegacyUpstreamTls, 500ms);
    result.resolver_calls = resolver->calls;
    if (!second) {
        result.error = second.error().io_error;
        result.error_code = second.error().code;
        result.second_observation = second.error().observation;
    } else {
        result.second_observation = second->observation;
        result.second_hit = second->lease.hit();
        second->lease.reset();
    }
    co_await pool->shutdown_async();
    promise->set_value(std::move(result));
}

fiber::async::DetachedTask run_shutdown_scenario(fiber::http::StealableHttp1ConnectionPoolSet *pool,
                                                 fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                                 std::promise<ConnectionScenarioResult> *promise) {
    co_await pool->shutdown_async();
    ConnectionScenarioResult result;
    auto connected = co_await fiber::access_server::acquire_proxy_upstream_connection(
            *pool, resolver_adapter(*resolver), key, kLegacyUpstreamTls, 500ms);
    result.resolver_calls = resolver->calls;
    if (!connected) {
        result.error = connected.error().io_error;
        result.error_code = connected.error().code;
        result.observation = connected.error().observation;
    }
    promise->set_value(std::move(result));
}

fiber::async::DetachedTask run_cross_worker_acquire(fiber::http::StealableHttp1ConnectionPoolSet *pool,
                                                    fiber::http::Http1ConnectionGroupKey key, ResolverState *resolver,
                                                    bool shutdown_pool,
                                                    std::promise<ConnectionScenarioResult> *promise) {
    ConnectionScenarioResult result;
    result.worker_index = fiber::event::EventLoop::current().group_index();
    auto connected = co_await fiber::access_server::acquire_proxy_upstream_connection(
            *pool, resolver_adapter(*resolver), key, kLegacyUpstreamTls, 500ms);
    result.resolver_calls = resolver->calls;
    if (!connected) {
        result.error = connected.error().io_error;
        result.error_code = connected.error().code;
        result.observation = connected.error().observation;
    } else {
        result.observation = connected->observation;
        result.first_hit = connected->lease.hit();
        result.connection_loop_index = connected->connection->loop().group_index();
        connected->lease.reset();
    }
    if (shutdown_pool) {
        co_await pool->shutdown_async();
    }
    promise->set_value(std::move(result));
}

ConnectionScenarioResult run_resolution_scenario(ResolverState *resolver) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    if (!pool.init()) {
        return ConnectionScenarioResult{.error = fiber::common::IoErr::NoMem};
    }
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", 80,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    if (!key) {
        return ConnectionScenarioResult{.error = fiber::common::IoErr::Invalid};
    }

    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_ip_scenario(&pool, *key, resolver, &promise); });
    ConnectionScenarioResult result = future.get();
    group.stop();
    group.join();
    return result;
}

TEST(ProxyUpstreamConnectionTest, IpKeyBypassesDns) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    fiber::net::TcpListener listener(group.at(0));
    ASSERT_TRUE(listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto port = bound_port(listener.fd());
    ASSERT_TRUE(port);

    ResolverState resolver;
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_ip_scenario(
                &pool,
                fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), *port,
                                                              fiber::http::Http1ConnectionGroupKey::Scheme::Http),
                &resolver, &promise);
    });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 0U);
    EXPECT_FALSE(result.first_hit);
    EXPECT_EQ(result.connected_ip, fiber::net::IpAddress::loopback_v4());
    EXPECT_EQ(result.observation.pool_misses, 1U);
    EXPECT_EQ(result.observation.connect_success, 1U);
    EXPECT_EQ(result.observation.dns_success, 0U);
    listener.close();
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, NameKeyTriesEveryResolvedAddress) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    fiber::net::TcpListener listener(group.at(0));
    ASSERT_TRUE(listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto port = bound_port(listener.fd());
    ASSERT_TRUE(port);
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", *port,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key);

    ResolverState resolver{
            .addresses =
                    {
                            fiber::net::IpAddress::v4({127, 0, 0, 2}),
                            fiber::net::IpAddress::loopback_v4(),
                    },
    };
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_ip_scenario(&pool, *key, &resolver, &promise); });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 1U);
    EXPECT_EQ(result.connected_ip, fiber::net::IpAddress::loopback_v4());
    EXPECT_EQ(result.observation.pool_misses, 2U);
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.connect_failure, 1U);
    EXPECT_EQ(result.observation.connect_success, 1U);
    listener.close();
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, ReportsBoundedDnsFailureOutcomes) {
    ResolverState empty;
    const ConnectionScenarioResult empty_result = run_resolution_scenario(&empty);
    EXPECT_EQ(empty_result.error_code, fiber::access_server::ProxyConnectErrorCode::Resolve);
    EXPECT_EQ(empty_result.error, fiber::common::IoErr::NotFound);
    EXPECT_EQ(empty_result.observation.pool_misses, 1U);
    EXPECT_EQ(empty_result.observation.dns_empty, 1U);

    ResolverState failed{
            .error = fiber::common::IoErr::TimedOut,
    };
    const ConnectionScenarioResult failed_result = run_resolution_scenario(&failed);
    EXPECT_EQ(failed_result.error_code, fiber::access_server::ProxyConnectErrorCode::Resolve);
    EXPECT_EQ(failed_result.error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(failed_result.observation.pool_misses, 1U);
    EXPECT_EQ(failed_result.observation.dns_failure, 1U);

    const ConnectionScenarioResult unavailable_result = run_resolution_scenario(nullptr);
    EXPECT_EQ(unavailable_result.error_code, fiber::access_server::ProxyConnectErrorCode::Resolve);
    EXPECT_EQ(unavailable_result.error, fiber::common::IoErr::NotFound);
    EXPECT_EQ(unavailable_result.observation.pool_misses, 1U);
    EXPECT_EQ(unavailable_result.observation.dns_unavailable, 1U);
}

TEST(ProxyUpstreamConnectionTest, PoolHitBypassesDns) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    fiber::net::TcpListener listener(group.at(0));
    ASSERT_TRUE(listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto port = bound_port(listener.fd());
    ASSERT_TRUE(port);
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", *port,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key);

    ResolverState resolver{
            .addresses = {fiber::net::IpAddress::loopback_v4()},
    };
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_pool_hit_scenario(&pool, *key, &resolver, &promise); });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 1U);
    EXPECT_FALSE(result.first_hit);
    EXPECT_TRUE(result.second_hit);
    EXPECT_EQ(result.observation.pool_misses, 1U);
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.connect_success, 1U);
    EXPECT_EQ(result.second_observation.pool_hits, 1U);
    EXPECT_EQ(result.second_observation.dns_success, 0U);
    EXPECT_EQ(result.second_observation.connect_success, 0U);
    listener.close();
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, StealsAnIdleConnectionFromAnotherWorker) {
    fiber::event::EventLoopGroup group(2);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    fiber::net::TcpListener listener(group.at(0));
    ASSERT_TRUE(listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto port = bound_port(listener.fd());
    ASSERT_TRUE(port);
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", *port,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key);

    ResolverState resolver{
            .addresses = {fiber::net::IpAddress::loopback_v4()},
    };
    group.start();

    std::promise<ConnectionScenarioResult> first_promise;
    auto first_future = first_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_cross_worker_acquire(&pool, *key, &resolver, false, &first_promise); });
    const ConnectionScenarioResult first = first_future.get();
    ASSERT_EQ(first.error, fiber::common::IoErr::None);

    std::promise<ConnectionScenarioResult> second_promise;
    auto second_future = second_promise.get_future();
    fiber::async::spawn(group.at(1),
                        [&]() { return run_cross_worker_acquire(&pool, *key, &resolver, true, &second_promise); });
    const ConnectionScenarioResult second = second_future.get();

    EXPECT_FALSE(first.first_hit);
    EXPECT_EQ(first.worker_index, 0U);
    EXPECT_EQ(first.connection_loop_index, 0U);
    EXPECT_TRUE(second.first_hit);
    EXPECT_EQ(second.worker_index, 1U);
    EXPECT_EQ(second.connection_loop_index, 0U);
    EXPECT_EQ(second.resolver_calls, 1U);
    EXPECT_EQ(first.observation.pool_misses, 1U);
    EXPECT_EQ(second.observation.pool_hits, 1U);

    listener.close();
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, ReportsPoolShutdownBeforeDns) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    auto key = fiber::http::Http1ConnectionGroupKey::from_name("upstream.example", 80,
                                                               fiber::http::Http1ConnectionGroupKey::Scheme::Http);
    ASSERT_TRUE(key);

    ResolverState resolver{
            .addresses = {fiber::net::IpAddress::loopback_v4()},
    };
    std::promise<ConnectionScenarioResult> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_shutdown_scenario(&pool, *key, &resolver, &promise); });

    const ConnectionScenarioResult result = future.get();
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::PoolShutdown);
    EXPECT_EQ(result.error, fiber::common::IoErr::Canceled);
    EXPECT_EQ(result.resolver_calls, 0U);
    EXPECT_EQ(result.observation.pool_shutdown, 1U);
    EXPECT_EQ(result.observation.pool_misses, 0U);
    EXPECT_EQ(result.observation.dns_success, 0U);
    group.stop();
    group.join();
}

TEST(ProxyUpstreamConnectionTest, ValidatesCustomTrustStoreBeforeRuntimeStart) {
    fiber::test::QuicTestTlsFile certificate("access-upstream-ca", fiber::test::kQuicTestCertificatePem);
    ASSERT_TRUE(certificate.valid());

    EXPECT_TRUE(fiber::access_server::validate_upstream_tls_client_policy({
            .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
            .ca_file = certificate.path(),
    }));
    auto missing = fiber::access_server::validate_upstream_tls_client_policy({
            .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
            .ca_file = "/missing/access-server-upstream-ca.pem",
    });
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), fiber::common::IoErr::Invalid);
}

TEST(ProxyUpstreamConnectionTest, LegacyModePreservesInsecureHttpsCompatibility) {
    fiber::test::QuicTestTlsFile certificate("access-upstream-legacy-cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-legacy-key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "untrusted.example", {});
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 1U);
    EXPECT_TRUE(result.tls_enabled);
    EXPECT_FALSE(result.verify_peer);
    EXPECT_TRUE(result.ca_file.empty());
    EXPECT_EQ(result.server_name, "untrusted.example");
    EXPECT_TRUE(result.verify_name.empty());
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.connect_success, 1U);
}

TEST(ProxyUpstreamConnectionTest, CustomCaVerifiesPeerAndDerivesSniFromUpstreamName) {
    fiber::test::QuicTestTlsFile certificate("access-upstream-custom-cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-custom-key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "localhost",
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
                                     .ca_file = certificate.path(),
                             });
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 1U);
    EXPECT_TRUE(result.tls_enabled);
    EXPECT_TRUE(result.verify_peer);
    EXPECT_EQ(result.ca_file, certificate.path());
    EXPECT_EQ(result.server_name, "localhost");
    EXPECT_TRUE(result.verify_name.empty());
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.connect_success, 1U);
}

TEST(ProxyUpstreamConnectionTest, CustomCaRejectsMismatchedCertificateNameAsTlsFailure) {
    fiber::test::QuicTestTlsFile certificate("access-upstream-name-cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-name-key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "wrong.example",
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
                                     .ca_file = certificate.path(),
                             });
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::Tls);
    EXPECT_EQ(result.error, fiber::common::IoErr::Invalid);
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.tls_failure, 1U);
    EXPECT_EQ(result.observation.connect_failure, 0U);
}

TEST(ProxyUpstreamConnectionTest, SystemCaRejectsPrivateSelfSignedCertificateAsTlsFailure) {
    fiber::test::QuicTestTlsFile certificate("access-upstream-system-cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-system-key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "localhost",
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::SystemCa,
                             });
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::Tls);
    EXPECT_EQ(result.error, fiber::common::IoErr::Invalid);
    EXPECT_EQ(result.observation.tls_failure, 1U);
}

TEST(ProxyUpstreamConnectionTest, VerifiedIpTargetRequiresCertificateIpIdentityWithoutIpSni) {
    fiber::test::QuicTestTlsFile certificate("access-upstream-ip-cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-ip-key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), {},
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
                                     .ca_file = certificate.path(),
                             },
                             true);
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::Tls);
    EXPECT_EQ(result.error, fiber::common::IoErr::Invalid);
    EXPECT_EQ(result.resolver_calls, 0U);
    EXPECT_EQ(result.observation.dns_success, 0U);
    EXPECT_EQ(result.observation.tls_failure, 1U);
}

} // namespace
