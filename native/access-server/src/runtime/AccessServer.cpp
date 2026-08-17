#include "AccessServer.h"
#include "../observability/AccessRuntimeMetrics.h"

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

http::HttpServerOptions make_http_options(http::HttpServerOptions options = {}) noexcept {
    options.drain_unread_body = true;
    return options;
}

} // namespace

AccessServer::AccessServer(event::EventLoop &accept_loop, event::EventLoopGroup &workers,
                           const RouteConfigStore &config_store, ProxyClusterMatcher cluster_matcher,
                           AccessServerOptions options) :
    accept_loop_(&accept_loop),
    worker_resources_(workers, config_store, cluster_matcher,
                      AccessWorkerResourcesOptions{
                              .default_max_request_body_size = options.default_max_request_body_size,
                              .client_metadata = std::move(options.client_metadata),
                              .connection_secure = options.http_server.tls.enabled,
                              .access_log = std::move(options.access_log),
                              .dns = std::move(options.dns),
                              .dns_resolver_factory = options.dns_resolver_factory,
                              .script_adapter = options.script_adapter,
                              .executor = std::move(options.executor),
                              .runtime_metrics = options.runtime_metrics,
                              .cat_client = options.cat_client,
                              .test_mode = options.test_mode,
                              .http3_alt_svc = std::move(options.http3_alt_svc),
                      }),
    server_(
            accept_loop, [this](http::HttpExchange &exchange) { return worker_resources_.handle(exchange); },
            make_http_options(std::move(options.http_server)), &workers),
    metrics_endpoint_(
            accept_loop, workers, worker_resources_.metrics(),
            AccessMetricsEndpointOptions{
                    .activation_evidence = options.activation_evidence,
                    .discovery_metrics = options.runtime_metrics ? &options.runtime_metrics->discovery() : nullptr,
                    .activation = std::move(options.activation_endpoint),
            }) {}

AccessServer::~AccessServer() { FIBER_ASSERT(!initialized_); }

async::Task<common::IoResult<void>> AccessServer::initialize() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (initialized_) {
        co_return std::unexpected(common::IoErr::Already);
    }
    auto initialized = co_await worker_resources_.initialize();
    if (!initialized) {
        co_return std::unexpected(initialized.error());
    }
    initialized_ = true;
    co_return common::IoResult<void>{};
}

common::IoResult<void> AccessServer::bind(const net::SocketAddress &address, const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(initialized_);
    return server_.bind(address, options);
}

common::IoResult<void> AccessServer::bind_metrics(const net::SocketAddress &address,
                                                  const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(initialized_);
    return metrics_endpoint_.bind(address, options);
}

async::DetachedTask AccessServer::serve() { return server_.serve(); }

async::DetachedTask AccessServer::serve_metrics() { return metrics_endpoint_.serve(); }

async::Task<void> AccessServer::shutdown_and_wait() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    co_await metrics_endpoint_.shutdown_and_wait();
    server_.close();
    co_await worker_resources_.shutdown();
    initialized_ = false;
}

} // namespace fiber::access_server
