#include "AccessServer.h"
#include "../observability/AccessRuntimeMetrics.h"

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

http::Http1ServerOptions make_http1_options() noexcept {
    http::Http1ServerOptions options;
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
                              .network_entry = std::move(options.network_entry),
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
    http_options_(options.http_server), plain_options_(options.plain_http_server),
    server_(accept_loop, http::HttpHandler{}, &workers), plain_server_(accept_loop, http::HttpHandler{}, &workers),
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
    if (main_bound_) {
        return std::unexpected(common::IoErr::Already);
    }
    if (http_options_.http3_enabled && !http_options_.tls.enabled()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    tls_endpoint_ = server_.add_endpoint<http::Http2Endpoint>(http::Http2Endpoint::Options{
            .address = address,
            .listen = options,
            .tls = http_options_.tls,
            // WebSocket over HTTP/2 (RFC 8441) arrives as an extended CONNECT,
            // which peers may only send once ENABLE_CONNECT_PROTOCOL is
            // advertised. Always on: routes still opt in per project through
            // websocketTimeoutMillis.
            .http2 = {.enable_connect_protocol = true},
            .http1 = make_http1_options(),
            .allow_http1 = true,
            .handler = [this](http::HttpExchange &exchange) { return worker_resources_.handle(exchange, true); },
    });
    if (tls_endpoint_ == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (http_options_.http3_enabled) {
        http3_endpoint_ = server_.add_endpoint<http::Http3Endpoint>(http::Http3Endpoint::Options{
                .address = address,
                .inherit_port_from = tls_endpoint_,
                .tls = http_options_.tls,
                // Same opt-in as the HTTP/2 endpoint, for WebSocket over
                // HTTP/3 (RFC 9220).
                .http3 = {.enable_connect_protocol = true},
                .handler = [this](http::HttpExchange &exchange) { return worker_resources_.handle(exchange, true); },
        });
        if (http3_endpoint_ == nullptr) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    auto started = server_.start();
    if (!started) {
        return std::unexpected(started.error());
    }
    main_bound_ = true;
    return {};
}

common::IoResult<void> AccessServer::bind_plain(const net::SocketAddress &address, const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(initialized_);
    if (plain_bound_) {
        return std::unexpected(common::IoErr::Already);
    }
    plain_endpoint_ = plain_server_.add_endpoint<http::Http1Endpoint>(http::Http1Endpoint::Options{
            .address = address,
            .listen = options,
            .http1 = make_http1_options(),
            .handler = [this](http::HttpExchange &exchange) { return worker_resources_.handle(exchange, false); },
    });
    if (plain_endpoint_ == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto started = plain_server_.start();
    if (!started) {
        return std::unexpected(started.error());
    }
    plain_bound_ = true;
    return {};
}

common::IoResult<void> AccessServer::bind_metrics(const net::SocketAddress &address,
                                                  const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(initialized_);
    return metrics_endpoint_.bind(address, options);
}

async::DetachedTask AccessServer::serve() {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (main_bound_) {
        serve_tasks_.add();
        async::spawn([this]() -> async::DetachedTask {
            co_await server_.serve();
            serve_tasks_.done();
        });
    }
    if (plain_bound_) {
        serve_tasks_.add();
        async::spawn([this]() -> async::DetachedTask {
            co_await plain_server_.serve();
            serve_tasks_.done();
        });
    }
    co_return;
}

async::DetachedTask AccessServer::serve_metrics() { return metrics_endpoint_.serve(); }

async::Task<void> AccessServer::shutdown_and_wait() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    co_await metrics_endpoint_.shutdown_and_wait();
    server_.stop();
    plain_server_.stop();
    co_await server_.stop_and_wait();
    co_await plain_server_.stop_and_wait();
    co_await serve_tasks_.join();
    co_await worker_resources_.shutdown();
    initialized_ = false;
}

} // namespace fiber::access_server
