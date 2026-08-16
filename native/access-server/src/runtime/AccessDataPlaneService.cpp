#include "AccessDataPlaneService.h"

#include "../observability/AccessRuntimeMetrics.h"
#include "TlsCertificateStore.h"

#include <new>
#include <string>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {

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
            .start = &start_lifecycle,
            .shutdown = &shutdown_lifecycle,
    };
}

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessDataPlaneService::start_lifecycle(void *context, AccessControlPlaneReady ready) noexcept {
    return static_cast<AccessDataPlaneService *>(context)->start(std::move(ready));
}

async::Task<void> AccessDataPlaneService::shutdown_lifecycle(void *context) noexcept {
    return static_cast<AccessDataPlaneService *>(context)->shutdown();
}

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessDataPlaneService::start(AccessControlPlaneReady ready) noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(!server_);

    if (options_.http_server.tls.enabled) {
        FIBER_ASSERT(ready.tls_bootstrap);
        options_.http_server.tls.cert_file = ready.tls_bootstrap->certificate_path();
        options_.http_server.tls.key_file = ready.tls_bootstrap->private_key_path();
        options_.http_server.tls.identity_selector_ops = ready.tls_identity_selector;
    }

    server_.reset(new (std::nothrow) AccessServer(
            *accept_loop_, *http_workers_, *route_store_, gray_matcher_,
            AccessServerOptions{
                    .default_max_request_body_size = options_.default_max_request_body_size,
                    .client_metadata = std::move(options_.client_metadata),
                    .access_log = std::move(options_.access_log),
                    .script_adapter = script_runtime_.request_adapter(),
                    .executor =
                            ProxyExecutorOptions{
                                    .upstream_tls = std::move(options_.upstream_tls),
                            },
                    .runtime_metrics = runtime_metrics_,
                    .activation_evidence = activation_evidence_,
                    .activation_endpoint = std::move(options_.activation_endpoint),
                    .cat_client = cat_client_,
                    .test_mode = options_.test_mode,
                    .http_server = options_.http_server,
                    .http3_alt_svc =
                            options_.http_server.http3.enabled
                                    ? "h3=\":" + std::to_string(options_.listen_address.port()) + "\"; ma=86400"
                                    : std::string{},
            }));
    if (!server_) {
        co_return std::unexpected(make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::InitializeWorkers,
                                                                      common::IoErr::NoMem,
                                                                      "failed to allocate access server"));
    }

    auto initialized = co_await server_->initialize();
    if (!initialized) {
        co_return std::unexpected(make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::InitializeWorkers,
                                                                      initialized.error()));
    }

    auto bound = server_->bind(options_.listen_address, options_.listen_options);
    if (ready.tls_bootstrap) {
        ready.tls_bootstrap->close();
    }
    if (!bound) {
        co_return std::unexpected(
                make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::Bind, bound.error()));
    }
    auto metrics_bound = server_->bind_metrics(options_.metrics_listen_address, options_.listen_options);
    if (!metrics_bound) {
        co_return std::unexpected(
                make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::BindMetrics, metrics_bound.error()));
    }

    async::spawn([this]() { return server_->serve(); });
    async::spawn([this]() { return server_->serve_metrics(); });
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<void> AccessDataPlaneService::shutdown() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (shutdown_complete_) {
        co_return;
    }
    if (server_) {
        co_await server_->shutdown_and_wait();
    }
    shutdown_complete_ = true;
}

} // namespace fiber::access_server
