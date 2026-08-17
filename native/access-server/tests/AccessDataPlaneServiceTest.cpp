#include "../src/runtime/AccessDataPlaneService.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>

#include "../src/observability/AccessRuntimeMetrics.h"
#include "../src/runtime/TlsCertificateStore.h"

namespace fiber::access_server {
namespace {

std::optional<net::SocketAddress> bound_address(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return std::nullopt;
    }
    net::SocketAddress result;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&address), length, result)) {
        return std::nullopt;
    }
    return result;
}

struct FailingDnsFactoryState {
    std::size_t calls = 0;
};

bool fail_dns_resolver(void *context, event::EventLoop &, dns::SharedDnsCache2 &, const dns::DnsClient::Options &,
                       std::unique_ptr<dns::DnsResolverLocal> &, std::unique_ptr<dns::DnsResolver> &) noexcept {
    ++static_cast<FailingDnsFactoryState *>(context)->calls;
    return false;
}

AccessDataPlaneOptions
data_plane_options(net::SocketAddress listen_address, net::SocketAddress metrics_listen_address,
                   AccessDnsResolverFactory dns_resolver_factory = AccessDnsResolverFactory::system()) {
    return AccessDataPlaneOptions{
            .listen_address = listen_address,
            .metrics_listen_address = metrics_listen_address,
            .dns_resolver_factory = dns_resolver_factory,
            .test_mode = true,
    };
}

class AccessDataPlaneServiceTest : public testing::Test {
protected:
    void SetUp() override { workers_.start(); }

    void TearDown() override {
        workers_.stop();
        workers_.join();
    }

    event::EventLoop accept_loop_;
    event::EventLoopGroup workers_{1};
    RouteConfigStore route_store_;
    AccessRuntimeMetrics runtime_metrics_{accept_loop_};
    AccessActivationEvidenceStore activation_evidence_{accept_loop_, AccessActivationEvidenceIdentity{
                                                                             .instance_id = "data-plane-test",
                                                                             .build_version = "test",
                                                                             .build_revision = "test-revision",
                                                                             .started_at_unix_millis = 1000,
                                                                     }};
};

TEST_F(AccessDataPlaneServiceTest, RollsBackWorkerInitializationFailureBeforeReturning) {
    auto bootstrap_result = TlsBootstrapIdentity::create("unused-certificate", "unused-private-key");
    ASSERT_TRUE(bootstrap_result);
    std::shared_ptr<TlsBootstrapIdentity> bootstrap = std::move(*bootstrap_result);
    const std::string certificate_path = bootstrap->certificate_path();
    const std::string private_key_path = bootstrap->private_key_path();
    ASSERT_EQ(::access(certificate_path.c_str(), F_OK), 0);
    ASSERT_EQ(::access(private_key_path.c_str(), F_OK), 0);
    bool completed = false;
    FailingDnsFactoryState dns_factory_state;
    async::spawn(accept_loop_, [&]() -> async::DetachedTask {
        const net::SocketAddress loopback(net::IpAddress::loopback_v4(), 0);
        AccessDataPlaneOptions options = data_plane_options(loopback, loopback,
                                                            AccessDnsResolverFactory{
                                                                    .context = &dns_factory_state,
                                                                    .create = fail_dns_resolver,
                                                            });
        options.http_server.tls.enabled = true;
        AccessDataPlaneService service(accept_loop_, workers_, route_store_, {}, runtime_metrics_, activation_evidence_,
                                       nullptr, std::move(options));

        auto started = co_await service.start(AccessControlPlaneReady{.tls_bootstrap = bootstrap});
        EXPECT_FALSE(started);
        if (!started) {
            EXPECT_EQ(started.error().code, AccessServerRuntimeErrorCode::InitializeWorkers);
        }
        EXPECT_EQ(dns_factory_state.calls, 1U);
        EXPECT_EQ(service.fd(), -1);
        EXPECT_EQ(service.metrics_fd(), -1);
        EXPECT_NE(::access(certificate_path.c_str(), F_OK), 0);
        EXPECT_NE(::access(private_key_path.c_str(), F_OK), 0);

        co_await service.shutdown();
        co_await service.shutdown();
        completed = true;
        accept_loop_.stop();
    });

    accept_loop_.run();
    EXPECT_TRUE(completed);
}

TEST_F(AccessDataPlaneServiceTest, RollsBackBusinessBindFailureBeforeReturning) {
    bool completed = false;
    async::spawn(accept_loop_, [&]() -> async::DetachedTask {
        net::TcpListener blocker(accept_loop_);
        auto blocked = blocker.bind(net::SocketAddress(net::IpAddress::loopback_v4(), 0), {});
        if (!blocked) {
            ADD_FAILURE() << "failed to reserve the business listener address";
            accept_loop_.stop();
            co_return;
        }
        const std::optional<net::SocketAddress> address = bound_address(blocker.fd());
        if (!address) {
            ADD_FAILURE() << "failed to read the reserved business listener address";
            blocker.close();
            accept_loop_.stop();
            co_return;
        }

        AccessDataPlaneService service(
                accept_loop_, workers_, route_store_, {}, runtime_metrics_, activation_evidence_, nullptr,
                data_plane_options(*address, net::SocketAddress(net::IpAddress::loopback_v4(), 0)));
        auto started = co_await service.start({});
        EXPECT_FALSE(started);
        if (!started) {
            EXPECT_EQ(started.error().code, AccessServerRuntimeErrorCode::Bind);
        }
        EXPECT_EQ(service.fd(), -1);
        EXPECT_EQ(service.metrics_fd(), -1);

        blocker.close();
        net::TcpListener rebound(accept_loop_);
        EXPECT_TRUE(rebound.bind(*address, {}));
        rebound.close();
        co_await service.shutdown();
        co_await service.shutdown();
        completed = true;
        accept_loop_.stop();
    });

    accept_loop_.run();
    EXPECT_TRUE(completed);
}

TEST_F(AccessDataPlaneServiceTest, RollsBackBusinessListenerWhenMetricsBindFails) {
    bool completed = false;
    async::spawn(accept_loop_, [&]() -> async::DetachedTask {
        net::TcpListener business_reservation(accept_loop_);
        auto reserved = business_reservation.bind(net::SocketAddress(net::IpAddress::loopback_v4(), 0), {});
        if (!reserved) {
            ADD_FAILURE() << "failed to reserve the business listener address";
            accept_loop_.stop();
            co_return;
        }
        const std::optional<net::SocketAddress> business_address = bound_address(business_reservation.fd());
        business_reservation.close();
        if (!business_address) {
            ADD_FAILURE() << "failed to read the reserved business listener address";
            accept_loop_.stop();
            co_return;
        }

        net::TcpListener metrics_blocker(accept_loop_);
        auto blocked = metrics_blocker.bind(net::SocketAddress(net::IpAddress::loopback_v4(), 0), {});
        if (!blocked) {
            ADD_FAILURE() << "failed to reserve the metrics listener address";
            accept_loop_.stop();
            co_return;
        }
        const std::optional<net::SocketAddress> metrics_address = bound_address(metrics_blocker.fd());
        if (!metrics_address) {
            ADD_FAILURE() << "failed to read the reserved metrics listener address";
            metrics_blocker.close();
            accept_loop_.stop();
            co_return;
        }

        AccessDataPlaneService service(accept_loop_, workers_, route_store_, {}, runtime_metrics_, activation_evidence_,
                                       nullptr, data_plane_options(*business_address, *metrics_address));
        auto started = co_await service.start({});
        EXPECT_FALSE(started);
        if (!started) {
            EXPECT_EQ(started.error().code, AccessServerRuntimeErrorCode::BindMetrics);
        }
        EXPECT_EQ(service.fd(), -1);
        EXPECT_EQ(service.metrics_fd(), -1);

        net::TcpListener rebound(accept_loop_);
        EXPECT_TRUE(rebound.bind(*business_address, {}));
        rebound.close();
        metrics_blocker.close();
        co_await service.shutdown();
        co_await service.shutdown();
        completed = true;
        accept_loop_.stop();
    });

    accept_loop_.run();
    EXPECT_TRUE(completed);
}

} // namespace
} // namespace fiber::access_server
