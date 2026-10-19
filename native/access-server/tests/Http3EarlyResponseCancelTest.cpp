// Reproduction of a production symptom seen on 172.28.2.219: an upstream
// (faas-server 172.28.2.126:16688, 25 MiB body cap) closes the connection
// while the gateway is still forwarding a large h3 upload. The gateway maps
// the failure to a final 502 (ProxyExecutor SendRequestBody ->
// SEND_REQUEST_ERROR) and returns from the handler long before the client
// finished sending the request body.
//
// Per RFC 9000 / RFC 9114 4.1 the server must then tell the client to stop
// uploading (STOP_SENDING with H3_REQUEST_CANCELLED), otherwise the client
// stays blocked on stream flow control forever: the gateway stopped reading,
// so no further MAX_STREAM_DATA can arrive, and Chrome keeps showing a
// stalled upload. ServerHttp3Request::run_read_loop queues that frame after
// the handler returns; these tests verify the frame actually reaches the
// client over a real loopback QUIC connection, in both shapes the production
// path produces (some body consumed by the proxy, and none at all).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp3Exchange.h>
#include <fiber/http/Http3Client.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/Server.h>
#include <fiber/http/endpoint/Http1Endpoint.h>
#include <fiber/http/endpoint/Http2Endpoint.h>
#include <fiber/http/endpoint/Http3Endpoint.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>
#include <fiber/quic/QuicUdpEndpoint.h>

#include "QuicTestTlsCertificate.h"

namespace {

using fiber::async::DetachedTask;
using fiber::http::Http1Endpoint;
using fiber::http::Http2Endpoint;
using fiber::http::Http3Endpoint;
using fiber::http::Server;
using namespace std::chrono_literals;

constexpr std::size_t kUploadTotal = 64U * 1024U * 1024U;
constexpr std::size_t kUploadChunk = 256U * 1024U;
constexpr std::size_t kServerReadsBeforeAbort = 1024U * 1024U;

struct TestCredential {
    fiber::test::QuicTestTlsFile cert{"h3-early-response-cert", fiber::test::kQuicTestCertificatePem};
    fiber::test::QuicTestTlsFile key{"h3-early-response-key", fiber::test::kQuicTestPrivateKeyPem};
    fiber::net::TlsCredential credential{};

    bool init() {
        if (!cert.valid() || !key.valid()) {
            return false;
        }
        fiber::net::TlsCredentialOptions options{};
        options.certificate_chain = fiber::net::TlsPemSource::from_file(cert.path());
        options.private_key = fiber::net::TlsPemSource::from_file(key.path());
        auto created = fiber::net::TlsCredential::create(options);
        if (!created) {
            return false;
        }
        credential = std::move(*created);
        return true;
    }
};

fiber::http::HttpServerTlsOptions tls_options(fiber::net::TlsCredential &credential) {
    fiber::http::HttpServerTlsOptions options{};
    options.configure_callback = &fiber::net::configure_tls_with_credential;
    options.configure_ctx = &credential;
    return options;
}

// Mirrors the gateway's error-response shape: a small final 502 body written
// while the request body is still in flight. `read_first` approximates how
// much of the upload the proxy forwarded before the upstream died.
fiber::async::Task<void> early_502(fiber::http::HttpExchange &exchange, std::size_t read_first) {
    std::size_t consumed = 0;
    while (consumed < read_first) {
        auto chunk = co_await exchange.read_body(64 * 1024, 2s);
        if (!chunk) {
            break;
        }
        consumed += chunk->readable_bytes();
        if (chunk->complete()) {
            break;
        }
    }

    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "text/plain");
    const std::string_view body = "upstream aborted";
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 502,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
            .end_stream = false,
    });
    if (!sent) {
        co_return;
    }
    (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true, 2s);
}

struct UploadOutcome {
    bool connected = false;
    fiber::common::IoErr head_error = fiber::common::IoErr::None;
    int status = 0;
    fiber::common::IoErr write_error = fiber::common::IoErr::None;
    std::size_t written = 0;
    bool write_completed = false;
    // Reader runs concurrently with the upload; set once read_header settles.
    fiber::common::IoErr concurrent_head_error = fiber::common::IoErr::None;
    int concurrent_status = 0;
    std::chrono::milliseconds concurrent_head_latency{};
};

// POSTs kUploadTotal bytes. Each write_all gets a short timeout: if the
// server never tells the client to stop, the upload surfaces as TimedOut
// once flow control closes; a delivered STOP_SENDING instead ends the write
// with a cancel/reset error quickly.
DetachedTask run_upload_client(fiber::event::EventLoop *loop, fiber::net::SocketAddress server_addr,
                               const std::string &cert_path, std::promise<UploadOutcome> *promise) {
    UploadOutcome outcome{};
    fiber::quic::QuicUdpEndpoint endpoint(*loop);
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    auto endpoint_ready = endpoint.init(endpoint_options);
    if (!endpoint_ready) {
        outcome.head_error = endpoint_ready.error();
        promise->set_value(std::move(outcome));
        co_return;
    }

    auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(cert_path));
    if (!trust_store) {
        outcome.head_error = trust_store.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::http::Http3Client::Options client_options{};
    client_options.tls.trust_store = trust_store->get();
    client_options.tls.verify_peer = true;
    fiber::http::Http3Client client(endpoint, std::move(client_options));
    auto started = endpoint.start();
    if (!started) {
        outcome.head_error = started.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::http::Http3ClientConnectOptions connect_options{};
    connect_options.remote_addr = server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 3s;
    fiber::http::Http3ClientConnection connection(client, connect_options);
    auto connected = co_await connection.connect();
    if (!connected) {
        outcome.head_error = connected.error().io_error;
        co_await endpoint.shutdown();
        promise->set_value(std::move(outcome));
        co_return;
    }
    outcome.connected = true;

    {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp3Exchange exchange = connection.open_exchange(pool);

        // Response reader racing the upload: the 502 must be readable while
        // the body write is still blocked on flow control. Joined before the
        // exchange leaves scope below: dropping the last stream lease tears a
        // still-parked read_header down as Canceled, discarding an
        // already-arrived head — a client-side lifecycle race that would fail
        // the concurrent-reader assertions below without a server defect.
        const auto started_at = std::chrono::steady_clock::now();
        fiber::async::WaitGroup reader_done;
        reader_done.add();
        fiber::async::spawn(*loop, [&exchange, &outcome, started_at, &reader_done]() -> DetachedTask {
            auto head = co_await exchange.read_header(10s);
            outcome.concurrent_head_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at);
            if (!head || *head == nullptr) {
                outcome.concurrent_head_error = head ? fiber::common::IoErr::Invalid : head.error();
            } else {
                outcome.concurrent_status = (*head)->status_code;
            }
            reader_done.done();
        });

        fiber::http::HttpHeaders headers(pool);
        headers.set("content-length", std::to_string(kUploadTotal));
        auto sent = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Post,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/upload",
                        .headers = &headers,
                },
                false, 3s);
        if (!sent) {
            outcome.head_error = sent.error();
        } else {
            static const auto chunk = [] {
                auto buffer = std::make_unique<std::uint8_t[]>(kUploadChunk);
                std::memset(buffer.get(), 'x', kUploadChunk);
                return buffer;
            }();
            while (outcome.write_error == fiber::common::IoErr::None && outcome.written < kUploadTotal) {
                const bool last = outcome.written + kUploadChunk >= kUploadTotal;
                const std::size_t len = last ? kUploadTotal - outcome.written : kUploadChunk;
                auto written = co_await exchange.write_all(chunk.get(), len, last, 5s);
                if (!written) {
                    outcome.write_error = written.error();
                    break;
                }
                outcome.written += *written;
            }
            outcome.write_completed = outcome.written == kUploadTotal;

            // The racing reader owns the response-head observation. Wait for
            // it to settle (head read, error, or its own 10s timeout) before
            // the block scope drops the exchange.
            co_await reader_done.join();
        }
    }

    connection.shutdown(fiber::http::Http3ErrorCode::NoError);
    co_await fiber::async::sleep(50ms);
    co_await endpoint.shutdown();
    co_await connection.wait_closed();
    promise->set_value(std::move(outcome));
    co_return;
}

void run_early_response_case(std::size_t server_read_first) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::promise<void> serve_done;
    Server server(group.at(0), fiber::http::HttpHandler{});
    Http3Endpoint *endpoint = server.add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(tls.credential),
            .handler = [server_read_first](
                               fiber::http::HttpExchange &exchange) { return early_502(exchange, server_read_first); },
    });
    ASSERT_NE(endpoint, nullptr);
    ASSERT_TRUE(server.start().has_value());
    fiber::async::spawn(group.at(0), [&server, &serve_done]() -> DetachedTask {
        co_await server.serve();
        serve_done.set_value();
        co_return;
    });

    const fiber::net::SocketAddress server_addr = endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<UploadOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, &tls, &promise]() {
        return run_upload_client(&client_group.at(0), server_addr, tls.cert.path(), &promise);
    });

    ASSERT_EQ(future.wait_for(30s), std::future_status::ready);
    const UploadOutcome outcome = future.get();
    printf("[outcome] connected=%d head_error=%d status=%d write_error=%d "
           "written=%zu completed=%d "
           "concurrent_head_error=%d concurrent_status=%d "
           "concurrent_head_latency=%lldms\n",
           static_cast<int>(outcome.connected), static_cast<int>(outcome.head_error), outcome.status,
           static_cast<int>(outcome.write_error), outcome.written, static_cast<int>(outcome.write_completed),
           static_cast<int>(outcome.concurrent_head_error), outcome.concurrent_status,
           static_cast<long long>(outcome.concurrent_head_latency.count()));

    server.stop();
    ASSERT_EQ(serve_done.get_future().wait_for(10s), std::future_status::ready);
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();

    ASSERT_TRUE(outcome.connected);
    EXPECT_EQ(outcome.concurrent_head_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.concurrent_status, 502);

    // The whole point: the upload must be cancelled by the server, not stall.
    // A STOP_SENDING that reaches the client ends the blocked write with a
    // cancel/reset; TimedOut means the client waited out flow control with
    // nobody reading — the reported production symptom.
    EXPECT_FALSE(outcome.write_completed);
    EXPECT_NE(outcome.write_error, fiber::common::IoErr::None);
    EXPECT_NE(outcome.write_error, fiber::common::IoErr::TimedOut)
            << "client upload stalled on flow control: no STOP_SENDING reached the "
               "client (written="
            << outcome.written << ")";
    EXPECT_LT(outcome.written, kUploadTotal);
}

// Production shape A: the proxy consumed ~25 MiB of the body before the
// upstream closed (CAT: phase=send_request_body io_error=broken_pipe).
TEST(Http3EarlyResponseCancelTest, Early502AfterPartialBodyReadCancelsUpload) {
    run_early_response_case(kServerReadsBeforeAbort);
}

// Production shape B: the upstream dies before the first body read completes;
// the handler never touches the request body.
TEST(Http3EarlyResponseCancelTest, Early502WithoutReadingBodyCancelsUpload) { run_early_response_case(0); }

// Long-lived server for shell-driven interop experiments: with
// H3_EARLY_RESPONSE_INTEROP_SERVER set, binds 127.0.0.1:18443. /upload runs
// the early-502 handler; any other path gets a 200 that logs the URI, so a
// browser test page can navigate to /result?... to report its outcome into
// the server log.
TEST(Http3EarlyResponseCancelTest, InteropServerMode) {
    if (getenv("H3_EARLY_RESPONSE_INTEROP_SERVER") == nullptr) {
        GTEST_SKIP() << "set H3_EARLY_RESPONSE_INTEROP_SERVER=1 to start the interop server";
    }

    TestCredential tls;
    ASSERT_TRUE(tls.init());

    constexpr std::uint16_t kPort = 18443;
    constexpr std::uint16_t kH2Port = 18444;

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<void> serve_done;
    Server server(group.at(0), fiber::http::HttpHandler{});
    fiber::http::HttpHandler interop_handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        const std::string_view path = exchange.uri().path;
        if (path == "/upload") {
            // Instrumented variant of early_502: the Chrome duplex
            // probe sends no content-length, so trace every read.
            // "?cap=N" bounds how much body is consumed before the
            // early 502, so a browser probe can cross the cancel
            // boundary at a size flow control still handles well.
            std::size_t cap = 1024 * 1024;
            bool drain = false;
            {
                const auto &uri = exchange.uri().unparsed_uri;
                const auto q = uri.find('?');
                if (q != std::string_view::npos) {
                    const auto rest = uri.substr(q);
                    const auto c = rest.find("cap=");
                    if (c != std::string_view::npos) {
                        std::size_t value = 0;
                        bool any = false;
                        for (const char ch: rest.substr(c + 4)) {
                            if (ch < '0' || ch > '9') {
                                break;
                            }
                            value = value * 10 + static_cast<std::size_t>(ch - '0');
                            any = true;
                        }
                        if (any) {
                            cap = value;
                        }
                    }
                    drain = rest.find("drain=1") != std::string_view::npos;
                }
            }
            printf("[interop] /upload start cap=%zu drain=%d\n", cap, static_cast<int>(drain));
            fflush(stdout);
            std::size_t consumed = 0;
            int reads = 0;
            while (consumed < cap) {
                const auto t0 = std::chrono::steady_clock::now();
                auto chunk = co_await exchange.read_body(64 * 1024, 5s);
                const auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
                if (!chunk) {
                    printf("[interop] read#%d err=%d after %lldms consumed=%zu\n", reads,
                           static_cast<int>(chunk.error()), static_cast<long long>(ms.count()), consumed);
                    fflush(stdout);
                    break;
                }
                consumed += chunk->readable_bytes();
                printf("[interop] read#%d bytes=%zu complete=%d %lldms consumed=%zu\n", ++reads,
                       chunk->readable_bytes(), static_cast<int>(chunk->complete()), static_cast<long long>(ms.count()),
                       consumed);
                fflush(stdout);
                if (chunk->complete()) {
                    break;
                }
            }
            printf("[interop] sending 502 after consumed=%zu\n", consumed);
            fflush(stdout);
            fiber::http::HttpHeaders headers(exchange.pool());
            headers.set("content-type", "text/plain");
            const std::string_view body = "upstream aborted";
            auto sent = co_await exchange.send_header({
                    .kind = fiber::http::OutgoingHeaderKind::Final,
                    .status_code = 502,
                    .headers = &headers,
                    .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
                    .end_stream = false,
            });
            printf("[interop] send_header ok=%d\n", static_cast<int>(sent.has_value()));
            fflush(stdout);
            if (sent) {
                (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(),
                                                   true, 2s);
            }
            if (drain) {
                // Drain variant: keep consuming (and discarding) the
                // rest of the body so the client's upload completes
                // normally and no STOP_SENDING is ever needed. This
                // is the comparison point that isolates whether the
                // client wedge is caused by the mid-body cancel or
                // by the early response itself.
                std::size_t drained = 0;
                int dreads = 0;
                while (true) {
                    auto chunk = co_await exchange.read_body(64 * 1024, 10s);
                    if (!chunk) {
                        printf("[interop] drain#%d err=%d drained=%zu\n", dreads, static_cast<int>(chunk.error()),
                               drained);
                        fflush(stdout);
                        break;
                    }
                    drained += chunk->readable_bytes();
                    const bool complete = chunk->complete();
                    if (++dreads % 16 == 0 || complete || drained > 4 * 1024 * 1024) {
                        printf("[interop] drain#%d bytes=%zu complete=%d drained=%zu\n", dreads,
                               chunk->readable_bytes(), static_cast<int>(complete), drained);
                        fflush(stdout);
                    }
                    if (complete) {
                        break;
                    }
                }
                printf("[interop] drain done drained=%zu\n", drained);
                fflush(stdout);
            }
            printf("[interop] handler done\n");
            fflush(stdout);
            co_return;
        }
        printf("[interop] %.*s\n", static_cast<int>(exchange.uri().unparsed_uri.size()),
               exchange.uri().unparsed_uri.data());
        fflush(stdout);
        fiber::http::HttpHeaders headers(exchange.pool());
        std::string_view body;
        // Same-origin upload probe page. Small POST first to
        // separate "any POST wedges" from "large body wedges",
        // then the large-body probe whose promise settles on
        // response headers while the upload may still stream.
        static constexpr std::string_view kPage =
                "<script>\n"
                "const TOTAL = 40 * 1024 * 1024;\n"
                "const start = performance.now();\n"
                "function report(q) {\n"
                "  console.log('page:' + q);\n"
                "  fetch('/result?ev=' + q + '&ms=' + (performance.now() - "
                "start).toFixed(0));\n"
                "}\n"
                "setInterval(() => console.log('page:alive ' + (performance.now() - "
                "start).toFixed(0)), 2000);\n"
                "function big(tag, body, cap, mode) {\n"
                "  const url = '/upload?cap=' + cap + (mode === 'drain' ? '&drain=1' : "
                "'');\n"
                "  if (mode === 'xhr') {\n"
                "    return new Promise((res, rej) => {\n"
                "      const x = new XMLHttpRequest();\n"
                "      x.open('POST', url);\n"
                "      x.onload = () => report(tag + '_done_' + x.status);\n"
                "      x.onerror = () => report(tag + '_error');\n"
                "      x.onabort = () => report(tag + '_abort');\n"
                "      x.send(body);\n"
                "    });\n"
                "  }\n"
                "  return fetch(url, {method: 'POST', body: body})\n"
                "    .then(r => report(tag + '_done_' + r.status))\n"
                "    .catch(e => report(tag + '_error_' + "
                "encodeURIComponent(e.name)));\n"
                "}\n"
                "// #size@cap[:mode],... — mode: '' (fetch), 'xhr', or 'drain'\n"
                "// (drain: server sends the early 502 but keeps consuming the\n"
                "// body, so no STOP_SENDING/reset happens mid-upload).\n"
                "const probes = (location.hash.length > 1 ? location.hash.slice(1) : "
                "'65536@65536')\n"
                "  .split(',')\n"
                "  .map(p => { const [s, c, m] = p.split('@'); return [Number(s), "
                "Number(c), m || '']; })\n"
                "  .filter(p => p[0] > 0 && p[1] > 0);\n"
                "let chain = Promise.resolve();\n"
                "for (const [s, c, m] of probes) {\n"
                "  chain = chain.then(() => big('sz' + s + 'cap' + c + 'm_' + (m || "
                "'fetch'), 'x'.repeat(s), c, m));\n"
                "}\n"
                "setTimeout(() => report('stuck'), 15000);\n"
                "setInterval(() => report('tick'), 5000);\n"
                "</script>";
        if (path == "/") {
            headers.set("content-type", "text/html");
            headers.set("alt-svc", "h3=\":18443\", h3=\":18443\"; ma=300");
            (void) co_await exchange.send_header({
                    .kind = fiber::http::OutgoingHeaderKind::Final,
                    .status_code = 200,
                    .headers = &headers,
                    .body = fiber::http::HttpBodySpec::ContentLength(kPage.size()),
                    .end_stream = false,
            });
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kPage.data()), kPage.size(), true,
                                               2s);
            co_return;
        }
        if (path == "/h2page") {
            // Same probe page over the h2 endpoint, without the
            // alt-svc header so the probes stay on h2 and never race
            // to the h3 port.
            static constexpr std::string_view kH2Page = kPage;
            headers.set("content-type", "text/html");
            (void) co_await exchange.send_header({
                    .kind = fiber::http::OutgoingHeaderKind::Final,
                    .status_code = 200,
                    .headers = &headers,
                    .body = fiber::http::HttpBodySpec::ContentLength(kH2Page.size()),
                    .end_stream = false,
            });
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kH2Page.data()), kH2Page.size(),
                                               true, 2s);
            co_return;
        }
        body = "ok";
        headers.set("content-type", "text/plain");
        (void) co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = &headers,
                .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
                .end_stream = false,
        });
        (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true, 2s);
        co_return;
    };
    // Same-port dual listeners, mirroring production: h1 over TCP for the
    // initial load (advertises Alt-Svc to h3), h3 over UDP for the probe.
    Http3Endpoint *endpoint = server.add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), kPort},
            .tls = tls_options(tls.credential),
            .handler = interop_handler,
    });
    ASSERT_NE(endpoint, nullptr);
    Http1Endpoint *tcp_endpoint = server.add_endpoint<Http1Endpoint>(Http1Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), kPort},
            .tls = tls_options(tls.credential),
            .handler = interop_handler,
    });
    ASSERT_NE(tcp_endpoint, nullptr);
    // h2 comparison endpoint: same handler over ALPN-negotiated HTTP/2, so
    // browser probes can contrast Chrome's handling of an early 502 +
    // RST_STREAM(CANCEL) against the h3 STOP_SENDING path.
    Http2Endpoint *h2_endpoint = server.add_endpoint<Http2Endpoint>(Http2Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), kH2Port},
            .tls = tls_options(tls.credential),
            .allow_http1 = true,
            .handler = interop_handler,
    });
    ASSERT_NE(h2_endpoint, nullptr);
    ASSERT_TRUE(server.start().has_value());
    fiber::async::spawn(group.at(0), [&server, &serve_done]() -> DetachedTask {
        co_await server.serve();
        serve_done.set_value();
        co_return;
    });

    printf("INTEROP_SERVER_READY\n");
    fflush(stdout);
    for (int i = 0; i < 150; ++i) {
        std::this_thread::sleep_for(1s);
    }

    server.stop();
    ASSERT_EQ(serve_done.get_future().wait_for(10s), std::future_status::ready);
    group.stop();
    group.join();
}

} // namespace
