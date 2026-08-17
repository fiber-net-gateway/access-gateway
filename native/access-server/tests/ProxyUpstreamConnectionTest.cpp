#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

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

struct TestTlsChain {
    std::string ca_certificate_pem;
    std::string server_certificate_pem;
    std::string server_private_key_pem;
};

std::optional<TestTlsChain> make_test_tls_chain(std::string_view ca_common_name) {
    using BasicConstraintsPtr = std::unique_ptr<BASIC_CONSTRAINTS, decltype(&BASIC_CONSTRAINTS_free)>;
    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using EcKeyPtr = std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)>;
    using GeneralNamePtr = std::unique_ptr<GENERAL_NAME, decltype(&GENERAL_NAME_free)>;
    using GeneralNamesPtr = std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)>;
    using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;

    const auto make_key = []() -> KeyPtr {
        EcKeyPtr ec_key(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), &EC_KEY_free);
        if (!ec_key || EC_KEY_generate_key(ec_key.get()) != 1) {
            return {nullptr, &EVP_PKEY_free};
        }
        KeyPtr key(EVP_PKEY_new(), &EVP_PKEY_free);
        if (!key || EVP_PKEY_assign_EC_KEY(key.get(), ec_key.get()) != 1) {
            return {nullptr, &EVP_PKEY_free};
        }
        ec_key.release();
        return key;
    };
    const auto initialize_certificate = [](X509 &certificate, EVP_PKEY &key, std::int64_t serial,
                                           std::string_view common_name) {
        if (X509_set_version(&certificate, X509_VERSION_3) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(&certificate), serial) != 1 ||
            X509_gmtime_adj(X509_get_notBefore(&certificate), -60) == nullptr ||
            X509_gmtime_adj(X509_get_notAfter(&certificate), 3600) == nullptr ||
            X509_set_pubkey(&certificate, &key) != 1) {
            return false;
        }
        X509_NAME *subject = X509_get_subject_name(&certificate);
        return subject != nullptr &&
               X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                          reinterpret_cast<const unsigned char *>(common_name.data()),
                                          static_cast<int>(common_name.size()), -1, 0) == 1;
    };
    const auto write_certificate = [](X509 &certificate) -> std::optional<std::string> {
        BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
        if (!bio || PEM_write_bio_X509(bio.get(), &certificate) != 1) {
            return std::nullopt;
        }
        char *data = nullptr;
        const long size = BIO_get_mem_data(bio.get(), &data);
        if (size <= 0 || data == nullptr) {
            return std::nullopt;
        }
        return std::string(data, static_cast<std::size_t>(size));
    };
    const auto write_private_key = [](EVP_PKEY &key) -> std::optional<std::string> {
        BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
        if (!bio || PEM_write_bio_PrivateKey(bio.get(), &key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            return std::nullopt;
        }
        char *data = nullptr;
        const long size = BIO_get_mem_data(bio.get(), &data);
        if (size <= 0 || data == nullptr) {
            return std::nullopt;
        }
        return std::string(data, static_cast<std::size_t>(size));
    };

    KeyPtr ca_key = make_key();
    X509Ptr ca_certificate(X509_new(), &X509_free);
    if (!ca_key || !ca_certificate || !initialize_certificate(*ca_certificate, *ca_key, 1, ca_common_name) ||
        X509_set_issuer_name(ca_certificate.get(), X509_get_subject_name(ca_certificate.get())) != 1) {
        return std::nullopt;
    }
    BasicConstraintsPtr ca_constraints(BASIC_CONSTRAINTS_new(), &BASIC_CONSTRAINTS_free);
    if (!ca_constraints) {
        return std::nullopt;
    }
    ca_constraints->ca = 0xff;
    if (X509_add1_ext_i2d(ca_certificate.get(), NID_basic_constraints, ca_constraints.get(), 1, 0) != 1 ||
        X509_sign(ca_certificate.get(), ca_key.get(), EVP_sha256()) <= 0) {
        return std::nullopt;
    }

    KeyPtr server_key = make_key();
    X509Ptr server_certificate(X509_new(), &X509_free);
    if (!server_key || !server_certificate ||
        !initialize_certificate(*server_certificate, *server_key, 2, "localhost") ||
        X509_set_issuer_name(server_certificate.get(), X509_get_subject_name(ca_certificate.get())) != 1) {
        return std::nullopt;
    }
    BasicConstraintsPtr server_constraints(BASIC_CONSTRAINTS_new(), &BASIC_CONSTRAINTS_free);
    GeneralNamesPtr server_names(GENERAL_NAMES_new(), &GENERAL_NAMES_free);
    GeneralNamePtr server_name(GENERAL_NAME_new(), &GENERAL_NAME_free);
    if (!server_constraints || !server_names || !server_name) {
        return std::nullopt;
    }
    server_constraints->ca = 0;
    server_name->type = GEN_DNS;
    server_name->d.dNSName = ASN1_IA5STRING_new();
    if (!server_name->d.dNSName || ASN1_STRING_set(server_name->d.dNSName, "localhost", 9) != 1 ||
        sk_GENERAL_NAME_push(server_names.get(), server_name.get()) <= 0) {
        return std::nullopt;
    }
    server_name.release();
    if (X509_add1_ext_i2d(server_certificate.get(), NID_basic_constraints, server_constraints.get(), 1, 0) != 1 ||
        X509_add1_ext_i2d(server_certificate.get(), NID_subject_alt_name, server_names.get(), 0, 0) != 1 ||
        X509_sign(server_certificate.get(), ca_key.get(), EVP_sha256()) <= 0) {
        return std::nullopt;
    }

    auto ca_pem = write_certificate(*ca_certificate);
    auto server_pem = write_certificate(*server_certificate);
    auto key_pem = write_private_key(*server_key);
    if (!ca_pem || !server_pem || !key_pem) {
        return std::nullopt;
    }
    return TestTlsChain{
            .ca_certificate_pem = std::move(*ca_pem),
            .server_certificate_pem = std::move(*server_pem),
            .server_private_key_pem = std::move(*key_pem),
    };
}

const std::optional<TestTlsChain> &trusted_test_tls_chain() {
    static const std::optional<TestTlsChain> chain = make_test_tls_chain("access-server test root");
    return chain;
}

const std::optional<TestTlsChain> &untrusted_test_tls_chain() {
    static const std::optional<TestTlsChain> chain = make_test_tls_chain("access-server untrusted root");
    return chain;
}

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
    fiber::common::IoErr server_error = fiber::common::IoErr::None;
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
    result.server_error = server_future.get();
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
    const auto &chain = trusted_test_tls_chain();
    ASSERT_TRUE(chain);
    fiber::test::QuicTestTlsFile ca_certificate("access-upstream-ca", chain->ca_certificate_pem);
    ASSERT_TRUE(ca_certificate.valid());

    EXPECT_TRUE(fiber::access_server::validate_upstream_tls_client_policy({
            .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
            .ca_file = ca_certificate.path(),
    }));
    auto missing = fiber::access_server::validate_upstream_tls_client_policy({
            .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
            .ca_file = "/missing/access-server-upstream-ca.pem",
    });
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), fiber::common::IoErr::Invalid);
}

TEST(ProxyUpstreamConnectionTest, LegacyModePreservesInsecureHttpsCompatibility) {
    const auto &chain = trusted_test_tls_chain();
    ASSERT_TRUE(chain);
    fiber::test::QuicTestTlsFile certificate("access-upstream-legacy-cert", chain->server_certificate_pem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-legacy-key", chain->server_private_key_pem);
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "untrusted.example", {});
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.server_error, fiber::common::IoErr::None);
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
    const auto &chain = trusted_test_tls_chain();
    ASSERT_TRUE(chain);
    fiber::test::QuicTestTlsFile ca_certificate("access-upstream-custom-ca", chain->ca_certificate_pem);
    fiber::test::QuicTestTlsFile certificate("access-upstream-custom-cert", chain->server_certificate_pem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-custom-key", chain->server_private_key_pem);
    ASSERT_TRUE(ca_certificate.valid());
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "localhost",
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
                                     .ca_file = ca_certificate.path(),
                             });
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.server_error, fiber::common::IoErr::None);
    EXPECT_EQ(result.resolver_calls, 1U);
    EXPECT_TRUE(result.tls_enabled);
    EXPECT_TRUE(result.verify_peer);
    EXPECT_EQ(result.ca_file, ca_certificate.path());
    EXPECT_EQ(result.server_name, "localhost");
    EXPECT_TRUE(result.verify_name.empty());
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.connect_success, 1U);
}

TEST(ProxyUpstreamConnectionTest, CustomCaRejectsMismatchedCertificateNameAsTlsFailure) {
    const auto &chain = trusted_test_tls_chain();
    ASSERT_TRUE(chain);
    fiber::test::QuicTestTlsFile ca_certificate("access-upstream-name-ca", chain->ca_certificate_pem);
    fiber::test::QuicTestTlsFile certificate("access-upstream-name-cert", chain->server_certificate_pem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-name-key", chain->server_private_key_pem);
    ASSERT_TRUE(ca_certificate.valid());
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "wrong.example",
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
                                     .ca_file = ca_certificate.path(),
                             });
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::Tls);
    EXPECT_EQ(result.error, fiber::common::IoErr::Invalid);
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.tls_failure, 1U);
    EXPECT_EQ(result.observation.connect_failure, 0U);
}

TEST(ProxyUpstreamConnectionTest, CustomCaRejectsCertificateFromUnknownAuthorityAsTlsFailure) {
    const auto &trusted_chain = trusted_test_tls_chain();
    const auto &untrusted_chain = untrusted_test_tls_chain();
    ASSERT_TRUE(trusted_chain);
    ASSERT_TRUE(untrusted_chain);
    fiber::test::QuicTestTlsFile trusted_ca("access-upstream-trusted-ca", trusted_chain->ca_certificate_pem);
    fiber::test::QuicTestTlsFile certificate("access-upstream-unknown-cert", untrusted_chain->server_certificate_pem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-unknown-key", untrusted_chain->server_private_key_pem);
    ASSERT_TRUE(trusted_ca.valid());
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), "localhost",
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
                                     .ca_file = trusted_ca.path(),
                             });
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::Tls);
    EXPECT_EQ(result.error, fiber::common::IoErr::Invalid);
    EXPECT_EQ(result.observation.dns_success, 1U);
    EXPECT_EQ(result.observation.tls_failure, 1U);
    EXPECT_EQ(result.observation.connect_failure, 0U);
}

TEST(ProxyUpstreamConnectionTest, SystemCaRejectsPrivateCertificateAuthorityAsTlsFailure) {
    const auto &chain = trusted_test_tls_chain();
    ASSERT_TRUE(chain);
    fiber::test::QuicTestTlsFile certificate("access-upstream-system-cert", chain->server_certificate_pem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-system-key", chain->server_private_key_pem);
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
    const auto &chain = trusted_test_tls_chain();
    ASSERT_TRUE(chain);
    fiber::test::QuicTestTlsFile ca_certificate("access-upstream-ip-ca", chain->ca_certificate_pem);
    fiber::test::QuicTestTlsFile certificate("access-upstream-ip-cert", chain->server_certificate_pem);
    fiber::test::QuicTestTlsFile private_key("access-upstream-ip-key", chain->server_private_key_pem);
    ASSERT_TRUE(ca_certificate.valid());
    ASSERT_TRUE(certificate.valid());
    ASSERT_TRUE(private_key.valid());

    const ConnectionScenarioResult result =
            run_tls_scenario(certificate.path(), private_key.path(), {},
                             {
                                     .verification = fiber::access_server::UpstreamTlsVerificationMode::CustomCa,
                                     .ca_file = ca_certificate.path(),
                             },
                             true);
    EXPECT_EQ(result.error_code, fiber::access_server::ProxyConnectErrorCode::Tls);
    EXPECT_EQ(result.error, fiber::common::IoErr::Invalid);
    EXPECT_EQ(result.resolver_calls, 0U);
    EXPECT_EQ(result.observation.dns_success, 0U);
    EXPECT_EQ(result.observation.tls_failure, 1U);
}

} // namespace
