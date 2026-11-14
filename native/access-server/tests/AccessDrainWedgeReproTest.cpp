// Reproduction harness for the 205 drain self-exit wedge: a generation that
// reaches zero TCP/UDP sockets after RELEASE still never exits, swallows
// SIGTERM (the main fiber is already past its signal wait), and pins the boss
// upgrade gate. This file drives the real Fiber HttpServer over real sockets
// with a handler parked the way the websocket relay parks: an upstream-less
// uplink leg blocked on exchange.read_body() with no timeout.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/Server.h>

#include <fiber/common/mem/BufPool.h>
#include <fiber/http/endpoint/Http1Endpoint.h>
#include <fiber/http/endpoint/Http2Endpoint.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpStream.h>

namespace {

using fiber::common::IoErr;

void log_line(const char *phase) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    std::printf("[%lld ms] %s\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()), phase);
}

// Mirrors the websocket relay's parked state: handshake head, then blocked on
// the client direction with no timeout (relay uplink leg). Status 101 takes the
// h1 upgrade shape (Raw tunnel body); 200 takes an h2 stream body.
fiber::async::Task<void> parked_websocket_handler(fiber::http::HttpExchange &exchange, std::atomic<bool> &done,
                                                  std::atomic<bool> &entered, int status_code) {
    struct DoneGuard {
        std::atomic<bool> &flag;
        ~DoneGuard() { flag.store(true, std::memory_order_release); }
    } guard{done};

    fiber::http::HttpHeaders headers(exchange.pool());
    fiber::http::HttpBodySpec body = fiber::http::HttpBodySpec::Stream();
    if (status_code == 101) {
        body = fiber::http::HttpBodySpec::Raw();
        (void) headers.set("Connection", "Upgrade");
        (void) headers.set("Upgrade", "websocket");
    }
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .reason = status_code == 101 ? "Switching Protocols" : std::string_view{},
            .headers = &headers,
            .body = body,
            .connection_mode = fiber::http::ResponseConnectionMode::Auto,
            .end_stream = false,
    });
    if (!sent) {
        log_line("handler: send_header failed");
        co_return;
    }
    entered.store(true, std::memory_order_release);
    for (;;) {
        auto chunk = co_await exchange.read_body(64 << 10);
        if (!chunk) {
            log_line("handler: read_body errored");
            co_return;
        }
        if (chunk->complete()) {
            log_line("handler: body complete");
            co_return;
        }
        auto written = co_await exchange.write_all(std::move(*chunk));
        if (!written) {
            log_line("handler: write_all errored");
            co_return;
        }
    }
}

} // namespace

// Prior-knowledge h2 (the plaintext twin of the TLS+h2 endpoint 205 serves):
// the handler answers, then parks on the request body with no timeout — the
// shape of a websocket uplink leg or a stalled upload. The client holds the
// stream open, the server drains, then the client connection dies.
TEST(AccessDrainWedgeReproTest, H2ParkedBodyDrainThenClientCloseExits) {
    fiber::event::EventLoopGroup group(2);
    group.start();
    fiber::event::EventLoop &owner = group.at(0);
    fiber::event::EventLoop &client_loop = group.at(1);

    std::atomic<bool> handler_entered{false};
    std::atomic<bool> handler_done{false};
    std::atomic<bool> client_got_head{false};
    std::atomic<bool> client_closed{false};
    std::atomic<bool> stop_done{false};
    std::atomic<bool> wedged{false};

    auto server = std::make_unique<fiber::http::Server>(owner, fiber::http::HttpHandler{}, &group);
    fiber::http::Http2Endpoint::Options options;
    options.address = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0);
    options.http2.enable_connect_protocol = true;
    options.allow_http1 = false;
    options.handler = [&handler_done, &handler_entered](fiber::http::HttpExchange &exchange) {
        return parked_websocket_handler(exchange, handler_done, handler_entered, 200);
    };
    auto *endpoint = server->add_endpoint<fiber::http::Http2Endpoint>(std::move(options));
    ASSERT_TRUE(server->start().has_value());
    const std::uint16_t port = endpoint->local_addr().port();
    ASSERT_NE(port, 0);
    fiber::async::spawn(owner, [raw = server.get()]() -> fiber::async::DetachedTask { co_await raw->serve(); });

    fiber::async::spawn(client_loop, [&]() -> fiber::async::DetachedTask {
        fiber::http::Http2ClientConnection connection(client_loop);
        auto connected = co_await connection.connect(
                fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), std::chrono::seconds(5));
        if (!connected) {
            ADD_FAILURE() << "client h2 connect failed";
            co_return;
        }
        std::atomic<bool> conn_closed{false};
        fiber::async::spawn(client_loop, [&connection, &conn_closed]() -> fiber::async::DetachedTask {
            co_await connection.wait_closed();
            conn_closed.store(true, std::memory_order_release);
        });
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp2Exchange exchange(connection, pool);
        // end_stream=false: the request body never arrives, so the server-side
        // handler parks on read_body exactly like a quiet websocket uplink.
        auto sent = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Post,
                        .scheme = "http",
                        .authority = "localhost",
                        .path = "/ws",
                },
                false);
        if (!sent) {
            ADD_FAILURE() << "client send_request_header failed";
            co_return;
        }
        auto head = co_await exchange.read_header(std::chrono::seconds(5));
        if (!head) {
            ADD_FAILURE() << "client read_header failed";
            co_return;
        }
        client_got_head.store(true, std::memory_order_release);
        log_line("client: h2 head received, idling");
        co_await fiber::async::sleep(std::chrono::milliseconds(1200));
        log_line("client: hard-dropping the h2 connection");
        connection.shutdown(fiber::common::IoErr::ConnReset);
        client_closed.store(true, std::memory_order_release);
        for (int i = 0; i < 50 && !conn_closed.load(std::memory_order_acquire); ++i) {
            co_await fiber::async::sleep(std::chrono::milliseconds(10));
        }
    });

    fiber::async::spawn(owner, [raw = server.get(), &stop_done]() -> fiber::async::DetachedTask {
        co_await fiber::async::sleep(std::chrono::milliseconds(300));
        log_line("drainer: begin stop_and_wait");
        co_await raw->stop_and_wait();
        stop_done.store(true, std::memory_order_release);
        log_line("drainer: stop_and_wait completed");
    });

    fiber::async::spawn(owner, [&]() -> fiber::async::DetachedTask {
        co_await fiber::async::sleep(std::chrono::seconds(6));
        if (!stop_done.load(std::memory_order_acquire)) {
            wedged.store(true, std::memory_order_release);
            log_line("watchdog: stop_and_wait did NOT complete - WEDGED");
        }
    });

    for (int waited = 0; waited < 100; ++waited) {
        if (stop_done.load(std::memory_order_acquire) || wedged.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(client_got_head.load(std::memory_order_acquire));
    EXPECT_TRUE(handler_entered.load(std::memory_order_acquire));
    EXPECT_TRUE(client_closed.load(std::memory_order_acquire));
    if (wedged.load(std::memory_order_acquire)) {
        FAIL() << "h2 stop_and_wait wedged: handler_done=" << handler_done.load(std::memory_order_acquire);
    }
    EXPECT_TRUE(handler_done.load(std::memory_order_acquire));
    EXPECT_TRUE(stop_done.load(std::memory_order_acquire));
    EXPECT_EQ(server->state(), fiber::http::Server::State::Stopped);

    if (!wedged.load(std::memory_order_acquire)) {
        server.reset();
    }
    group.stop();
    group.join();
}

TEST(AccessDrainWedgeReproTest, H1WebsocketDrainThenClientCloseExits) {
    fiber::event::EventLoopGroup group(2);
    group.start();
    fiber::event::EventLoop &owner = group.at(0);
    fiber::event::EventLoop &client_loop = group.at(1);

    std::atomic<bool> handler_entered{false};
    std::atomic<bool> handler_done{false};
    std::atomic<bool> client_got_101{false};
    std::atomic<bool> client_closed{false};
    std::atomic<bool> stop_done{false};
    std::atomic<bool> wedged{false};

    auto server = std::make_unique<fiber::http::Server>(owner, fiber::http::HttpHandler{}, &group);
    fiber::http::Http1Endpoint::Options options;
    options.address = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0);
    options.http1.drain_unread_body = true;
    options.handler = [&handler_done, &handler_entered](fiber::http::HttpExchange &exchange) {
        return parked_websocket_handler(exchange, handler_done, handler_entered, 101);
    };
    auto *endpoint = server->add_endpoint<fiber::http::Http1Endpoint>(std::move(options));
    ASSERT_TRUE(server->start().has_value());
    const std::uint16_t port = endpoint->local_addr().port();
    ASSERT_NE(port, 0);
    fiber::async::spawn(owner, [raw = server.get()]() -> fiber::async::DetachedTask { co_await raw->serve(); });

    // Client: open the websocket, then idle (a quiet websocket session).
    fiber::async::spawn(client_loop, [&]() -> fiber::async::DetachedTask {
        auto connected = co_await fiber::net::TcpStream::connect(
                client_loop, fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port),
                std::chrono::seconds(5));
        if (!connected) {
            ADD_FAILURE() << "client connect failed";
            co_return;
        }
        fiber::net::TcpStream stream(std::move(*connected));
        const std::string_view request = "GET /ws HTTP/1.1\r\n"
                                         "Host: localhost\r\n"
                                         "Connection: Upgrade\r\n"
                                         "Upgrade: websocket\r\n"
                                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                         "Sec-WebSocket-Version: 13\r\n"
                                         "\r\n";
        auto written = co_await stream.write(request.data(), request.size(), std::chrono::seconds(5));
        if (!written) {
            ADD_FAILURE() << "client write failed";
            co_return;
        }
        std::string received;
        while (received.find("\r\n\r\n") == std::string::npos) {
            std::uint8_t buffer[512];
            auto chunk = co_await stream.read(buffer, sizeof(buffer), std::chrono::seconds(5));
            if (!chunk || *chunk == 0) {
                ADD_FAILURE() << "client read failed before 101";
                co_return;
            }
            received.append(reinterpret_cast<char *>(buffer), *chunk);
        }
        if (received.find("101") == std::string::npos) {
            ADD_FAILURE() << "client did not receive 101: " << received.substr(0, 32);
            co_return;
        }
        client_got_101.store(true, std::memory_order_release);
        log_line("client: 101 received, idling");
        // Hold the session open across the drain start.
        co_await fiber::async::sleep(std::chrono::milliseconds(1200));
        log_line("client: closing socket");
        stream.close();
        client_closed.store(true, std::memory_order_release);
    });

    // Drainer: stop() while the websocket session is alive, then wait.
    fiber::async::spawn(owner, [raw = server.get(), &stop_done]() -> fiber::async::DetachedTask {
        co_await fiber::async::sleep(std::chrono::milliseconds(300));
        log_line("drainer: begin stop_and_wait");
        co_await raw->stop_and_wait();
        stop_done.store(true, std::memory_order_release);
        log_line("drainer: stop_and_wait completed");
    });

    // Watchdog: if stop_and_wait never completes, record it and unblock the
    // test so the failure reports instead of hanging.
    fiber::async::spawn(owner, [&]() -> fiber::async::DetachedTask {
        co_await fiber::async::sleep(std::chrono::seconds(6));
        if (!stop_done.load(std::memory_order_acquire)) {
            wedged.store(true, std::memory_order_release);
            log_line("watchdog: stop_and_wait did NOT complete - WEDGED");
        }
    });

    for (int waited = 0; waited < 100; ++waited) {
        if (stop_done.load(std::memory_order_acquire) || wedged.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(client_got_101.load(std::memory_order_acquire));
    EXPECT_TRUE(handler_entered.load(std::memory_order_acquire));
    EXPECT_TRUE(client_closed.load(std::memory_order_acquire));
    if (wedged.load(std::memory_order_acquire)) {
        FAIL() << "stop_and_wait wedged with zero sockets: handler_done="
               << handler_done.load(std::memory_order_acquire);
    }
    EXPECT_TRUE(handler_done.load(std::memory_order_acquire));
    EXPECT_TRUE(stop_done.load(std::memory_order_acquire));
    EXPECT_EQ(server->state(), fiber::http::Server::State::Stopped);

    if (!wedged.load(std::memory_order_acquire)) {
        server.reset();
    }
    group.stop();
    group.join();
}
