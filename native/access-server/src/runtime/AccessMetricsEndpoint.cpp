#include "AccessMetricsEndpoint.h"

#include <string_view>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

http::HttpServerOptions metrics_http_options() noexcept {
    http::HttpServerOptions options;
    options.drain_unread_body = true;
    return options;
}

} // namespace

AccessMetricsEndpoint::AccessMetricsEndpoint(event::EventLoop &accept_loop, event::EventLoopGroup &workers,
                                             AccessServerMetrics &metrics, AccessMetricsEndpointOptions options) :
    accept_loop_(&accept_loop), metrics_(&metrics),
    activation_endpoint_(options.activation_evidence, options.discovery_metrics, std::move(options.activation)),
    server_(
            accept_loop, [this](http::HttpExchange &exchange) { return handle(exchange); }, metrics_http_options(),
            &workers) {}

AccessMetricsEndpoint::~AccessMetricsEndpoint() { FIBER_ASSERT(!bound_); }

common::IoResult<void> AccessMetricsEndpoint::bind(const net::SocketAddress &address,
                                                   const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    auto bound = server_.bind(address, options);
    if (bound) {
        bound_ = true;
    }
    return bound;
}

async::DetachedTask AccessMetricsEndpoint::serve() { return server_.serve(); }

async::Task<void> AccessMetricsEndpoint::shutdown_and_wait() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (!bound_) {
        co_return;
    }
    co_await server_.shutdown_and_wait();
    bound_ = false;
}

async::Task<void> AccessMetricsEndpoint::handle(http::HttpExchange &exchange) noexcept {
    if (exchange.uri().path != "/metrics") {
        co_await activation_endpoint_.handle(exchange);
        co_return;
    }
    auto collected = co_await metrics_->collect(event::EventLoop::current().io_buf_node_pool());
    if (!collected) {
        constexpr std::string_view kBusy = "metrics unavailable\n";
        http::HttpHeaders headers(exchange.pool());
        headers.set_view("Content-Type", "text/plain; charset=utf-8");
        auto sent = co_await exchange.send_header({
                .kind = http::OutgoingHeaderKind::Final,
                .status_code = collected.error() == common::IoErr::Busy ? 503 : 500,
                .headers = &headers,
                .body = http::HttpBodySpec::ContentLength(kBusy.size()),
                .connection_mode = http::ResponseConnectionMode::Auto,
                .end_stream = false,
        });
        if (sent) {
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kBusy.data()), kBusy.size(),
                                               true);
        }
        co_return;
    }

    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
    const std::size_t size = collected->readable_bytes();
    auto sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(size),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = size == 0,
    });
    if (sent && size != 0) {
        collected->mark_complete();
        (void) co_await exchange.write_all(std::move(*collected));
    }
}

} // namespace fiber::access_server
