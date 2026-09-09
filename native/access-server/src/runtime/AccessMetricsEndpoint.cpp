#include "AccessMetricsEndpoint.h"

#include <string_view>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <fiber/http/Http1ServerOptions.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

http::Http1ServerOptions metrics_http1_options() noexcept {
    http::Http1ServerOptions options;
    options.drain_unread_body = true;
    return options;
}

} // namespace

AccessMetricsEndpoint::AccessMetricsEndpoint(event::EventLoop &accept_loop, event::EventLoopGroup &workers,
                                             AccessServerMetrics &metrics, AccessMetricsEndpointOptions options) :
    accept_loop_(&accept_loop), metrics_(&metrics),
    activation_endpoint_(options.activation_evidence, options.discovery_metrics, std::move(options.activation)),
    server_(accept_loop, http::HttpHandler{}, &workers) {}

AccessMetricsEndpoint::~AccessMetricsEndpoint() { FIBER_ASSERT(!bound_); }

common::IoResult<void> AccessMetricsEndpoint::bind(const net::SocketAddress &address,
                                                   const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (bound_) {
        return std::unexpected(common::IoErr::Already);
    }
    endpoint_ = server_.add_endpoint<http::Http1Endpoint>(http::Http1Endpoint::Options{
            .address = address,
            .listen = options,
            .http1 = metrics_http1_options(),
            .handler = [this](http::HttpExchange &exchange) { return handle(exchange); },
    });
    if (endpoint_ == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto started = server_.start();
    if (!started) {
        return std::unexpected(started.error());
    }
    bound_ = true;
    return {};
}

async::DetachedTask AccessMetricsEndpoint::serve() {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (!bound_) {
        co_return;
    }
    serve_tasks_.add();
    async::spawn([this]() -> async::DetachedTask {
        co_await server_.serve();
        serve_tasks_.done();
    });
    co_return;
}

async::Task<void> AccessMetricsEndpoint::shutdown_and_wait() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (!bound_) {
        co_return;
    }
    server_.stop();
    co_await server_.stop_and_wait();
    co_await serve_tasks_.join();
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
