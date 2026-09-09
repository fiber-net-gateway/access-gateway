#include "AccessDataPlaneService.h"

#include "../observability/AccessRuntimeMetrics.h"
#include "TlsCertificateStore.h"

#include <new>
#include <string>
#include <utility>

#include <cerrno>
#include <sys/socket.h>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

common::IoResult<net::SocketAddress> bound_address(int fd) noexcept {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    net::SocketAddress result;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&address), length, result)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return result;
}

} // namespace

AccessDataPlaneService::AccessDataPlaneService(event::EventLoop &accept_loop, event::EventLoopGroup &http_workers,
                                               const RouteConfigStore &route_store, ProxyClusterMatcher gray_matcher,
                                               const AccessRuntimeMetrics &runtime_metrics,
                                               const AccessActivationEvidenceStore &activation_evidence,
                                               cat::CatClient *cat_client, AccessDataPlaneOptions options) noexcept :
    accept_loop_(&accept_loop), http_workers_(&http_workers), route_store_(&route_store),
    runtime_metrics_(&runtime_metrics), activation_evidence_(&activation_evidence), cat_client_(cat_client),
    gray_matcher_(gray_matcher), options_(std::move(options)) {}

AccessDataPlaneService::~AccessDataPlaneService() noexcept = default;

AccessDataPlaneLifecycle AccessDataPlaneService::lifecycle() noexcept {
    return AccessDataPlaneLifecycle{
            .context = this,
            .bind = &bind_lifecycle,
            .serve = &serve_lifecycle,
            .shutdown = &shutdown_lifecycle,
    };
}

async::Task<std::expected<AccessBoundEndpoint, AccessServerRuntimeError>>
AccessDataPlaneService::bind_lifecycle(void *context, AccessControlPlaneReady ready) noexcept {
    return static_cast<AccessDataPlaneService *>(context)->bind(std::move(ready));
}

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessDataPlaneService::serve_lifecycle(void *context) noexcept {
    return static_cast<AccessDataPlaneService *>(context)->serve();
}

async::Task<void> AccessDataPlaneService::shutdown_lifecycle(void *context) noexcept {
    return static_cast<AccessDataPlaneService *>(context)->shutdown();
}

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessDataPlaneService::start(AccessControlPlaneReady ready) noexcept {
    auto bound = co_await bind(std::move(ready));
    if (!bound) {
        co_return std::unexpected(std::move(bound.error()));
    }
    co_return co_await serve();
}

async::Task<std::expected<AccessBoundEndpoint, AccessServerRuntimeError>>
AccessDataPlaneService::bind(AccessControlPlaneReady ready) noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(!server_);
    FIBER_ASSERT(!shutdown_complete_);

    if (options_.http_server.tls.enabled()) {
        FIBER_ASSERT(ready.tls_bootstrap);
        FIBER_ASSERT(ready.tls_configure_callback);
        options_.http_server.tls.configure_callback = ready.tls_configure_callback;
        options_.http_server.tls.configure_ctx = ready.tls_configure_context;
    }

    server_.reset(new (std::nothrow) AccessServer(
            *accept_loop_, *http_workers_, *route_store_, gray_matcher_,
            AccessServerOptions{
                    .default_max_request_body_size = options_.default_max_request_body_size,
                    .client_metadata = std::move(options_.client_metadata),
                    .network_entry = std::move(options_.network_entry),
                    .access_log = std::move(options_.access_log),
                    .dns = std::move(options_.dns),
                    .dns_resolver_factory = options_.dns_resolver_factory,
                    .script_adapter = script_runtime_.request_adapter(),
                    .executor = std::move(options_.executor),
                    .runtime_metrics = runtime_metrics_,
                    .activation_evidence = activation_evidence_,
                    .activation_endpoint = std::move(options_.activation_endpoint),
                    .cat_client = cat_client_,
                    .test_mode = options_.test_mode,
                    .http_server = options_.http_server,
                    .http3_alt_svc =
                            options_.http_server.http3_enabled
                                    ? "h3=\":" + std::to_string(options_.listen_address.port()) + "\"; ma=86400"
                                    : std::string{},
                    .plain_listen_enabled = options_.plain_listen_enabled,
                    .plain_http_server = std::move(options_.plain_http_server),
            }));
    if (!server_) {
        co_await rollback_start(ready);
        co_return std::unexpected(make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::InitializeWorkers,
                                                                      common::IoErr::NoMem,
                                                                      "failed to allocate access server"));
    }

    auto initialized = co_await server_->initialize();
    if (!initialized) {
        co_await rollback_start(ready);
        co_return std::unexpected(make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::InitializeWorkers,
                                                                      initialized.error()));
    }

    // The TLS listener is only bound when TLS is enabled; with TLS off the
    // TLS address:port stays closed and only the plaintext listener serves.
    if (options_.http_server.tls.enabled()) {
        auto bound = server_->bind(options_.listen_address, options_.listen_options);
        if (ready.tls_bootstrap) {
            ready.tls_bootstrap->close();
        }
        if (!bound) {
            co_await rollback_start(ready);
            co_return std::unexpected(
                    make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::Bind, bound.error()));
        }
    } else if (ready.tls_bootstrap) {
        ready.tls_bootstrap->close();
    }
    if (options_.plain_listen_enabled) {
        auto plain_bound = server_->bind_plain(options_.plain_listen_address, options_.listen_options);
        if (!plain_bound) {
            co_await rollback_start(ready);
            co_return std::unexpected(
                    make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::Bind, plain_bound.error()));
        }
    }
    auto metrics_bound = server_->bind_metrics(options_.metrics_listen_address, options_.listen_options);
    if (!metrics_bound) {
        co_await rollback_start(ready);
        co_return std::unexpected(
                make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::BindMetrics, metrics_bound.error()));
    }

    const bool use_tls = options_.registration_listener == AccessRegistrationListener::Tls;
    const int registration_fd = use_tls ? server_->fd() : server_->plain_fd();
    auto endpoint = bound_address(registration_fd);
    if (!endpoint) {
        co_await rollback_start(ready);
        co_return std::unexpected(make_access_server_runtime_io_error(
                AccessServerRuntimeErrorCode::ResolveBoundListener, endpoint.error(),
                "failed to resolve the bound registration listener"));
    }
    bound_ = true;
    co_return AccessBoundEndpoint{.address = *endpoint, .tls = use_tls};
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessDataPlaneService::serve() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(bound_);
    FIBER_ASSERT(server_);
    if (serving_) {
        co_return std::unexpected(make_access_server_runtime_io_error(
                AccessServerRuntimeErrorCode::Serve, common::IoErr::Already, "data plane is already serving"));
    }
    async::spawn([this]() { return server_->serve(); });
    async::spawn([this]() { return server_->serve_metrics(); });
    serving_ = true;
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<void> AccessDataPlaneService::rollback_start(AccessControlPlaneReady &ready) noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (ready.tls_bootstrap) {
        ready.tls_bootstrap->close();
    }
    if (server_) {
        co_await server_->shutdown_and_wait();
    }
    bound_ = false;
    serving_ = false;
    shutdown_complete_ = true;
}

async::Task<void> AccessDataPlaneService::shutdown() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (shutdown_complete_) {
        co_return;
    }
    if (server_) {
        co_await server_->shutdown_and_wait();
    }
    bound_ = false;
    serving_ = false;
    shutdown_complete_ = true;
}

} // namespace fiber::access_server
