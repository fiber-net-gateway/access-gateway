#include "BenchmarkAllocationProbe.h"
#include "BenchmarkSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <netinet/in.h>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpServer.h>
#include <fiber/http/StealableHttp1ConnectionPoolSet.h>
#include <fiber/log/LogConfig.h>
#include <fiber/log/LoggerManager.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

#include "execution/AccessRequestHandler.h"
#include "execution/ClientMetadata.h"
#include "execution/ProxyExecutor.h"
#include "observability/AccessRequestTelemetry.h"
#include "runtime/RouteConfigStore.h"

namespace {

using namespace std::chrono_literals;

using fiber::access_server::benchmark::AllocationMeasurement;
using fiber::access_server::benchmark::Distribution;

constexpr std::uint64_t kDefaultHttpRequestsPerSample = 1'000;
constexpr std::uint64_t kDefaultWebSocketSessions = 101;
constexpr std::string_view kHttpResponseBody = "benchmark-response";
constexpr std::string_view kWebSocketServerFrame = "server-frame";
constexpr std::string_view kWebSocketClientFrame = "client-frame";

class SilentLogging {
public:
    SilentLogging() {
        fiber::log::LogConfigBuilder builder;
        auto root = builder.set_root_logger({.level = fiber::log::LogLevel::Fatal}, {});
        auto config = root ? builder.finish()
                           : fiber::log::LogConfigResult<fiber::log::LogConfig>(std::unexpected(root.error()));
        if (config) {
            initialized_ = fiber::log::LoggerManager::global().initialize(std::move(*config)).has_value();
        }
    }

    ~SilentLogging() { fiber::log::LoggerManager::global().shutdown(); }

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    bool initialized_ = false;
};

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const unsigned char left_char = static_cast<unsigned char>(left[index]);
        const unsigned char right_char = static_cast<unsigned char>(right[index]);
        const unsigned char normalized_left =
                left_char >= 'A' && left_char <= 'Z' ? static_cast<unsigned char>(left_char + ('a' - 'A')) : left_char;
        const unsigned char normalized_right = right_char >= 'A' && right_char <= 'Z'
                                                       ? static_cast<unsigned char>(right_char + ('a' - 'A'))
                                                       : right_char;
        if (normalized_left != normalized_right) {
            return false;
        }
    }
    return true;
}

bool header_contains_token(std::string_view input, std::string_view expected) noexcept {
    std::size_t offset = 0;
    while (offset <= input.size()) {
        const std::size_t comma = input.find(',', offset);
        std::string_view token =
                input.substr(offset, comma == std::string_view::npos ? input.size() - offset : comma - offset);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (ascii_iequals(token, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return false;
}

std::string consume_chain(fiber::mem::IoBufChain chain) {
    std::string result;
    while (fiber::mem::IoBuf *part = chain.first_readable()) {
        result.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
        chain.consume_and_compact(part->readable());
    }
    return result;
}

fiber::common::IoResult<std::uint16_t> bound_port(int fd) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

struct UpstreamState {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> connections{0};
    std::atomic<std::uint64_t> websocket_sessions{0};
    std::atomic<std::uint16_t> last_remote_port{0};
};

void observe_connection(UpstreamState &state, std::uint16_t remote_port) noexcept {
    const std::uint16_t previous = state.last_remote_port.exchange(remote_port, std::memory_order_relaxed);
    if (previous != remote_port) {
        state.connections.fetch_add(1, std::memory_order_relaxed);
    }
}

fiber::async::Task<void> serve_upstream(fiber::http::HttpExchange &exchange, UpstreamState *state) {
    state->requests.fetch_add(1, std::memory_order_relaxed);
    observe_connection(*state, exchange.remote_addr().port());
    const bool websocket = header_contains_token(exchange.header("Connection"), "upgrade") &&
                           ascii_iequals(exchange.header("Upgrade"), "websocket");
    if (websocket) {
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("Connection", "Upgrade");
        headers.set("Upgrade", "websocket");
        headers.set("Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        auto sent = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 101,
                .headers = &headers,
                .body = fiber::http::HttpBodySpec::Stream(),
                .end_stream = false,
        });
        if (sent) {
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kWebSocketServerFrame.data()),
                                               kWebSocketServerFrame.size(), false);
            auto client_frame = co_await exchange.read_body(64 * 1024);
            if (client_frame && consume_chain(std::move(*client_frame)) == kWebSocketClientFrame) {
                state->websocket_sessions.fetch_add(1, std::memory_order_relaxed);
            }
            (void) co_await exchange.write_all(nullptr, 0, true);
        }
        co_return;
    }

    for (;;) {
        auto body = co_await exchange.read_body(64 * 1024);
        if (!body || body->complete()) {
            break;
        }
    }
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(kHttpResponseBody.size()),
            .end_stream = false,
    });
    if (sent) {
        (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kHttpResponseBody.data()),
                                           kHttpResponseBody.size(), true);
    }
}

fiber::async::DetachedTask start_server(fiber::event::EventLoop *loop, fiber::http::HttpHandler handler,
                                        std::promise<std::uint16_t> *port_promise,
                                        std::promise<fiber::http::HttpServer *> *server_promise) {
    auto *server = new (std::nothrow) fiber::http::HttpServer(*loop, std::move(handler));
    if (server == nullptr) {
        port_promise->set_value(0);
        server_promise->set_value(nullptr);
        co_return;
    }
    auto bound = server->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    const auto port =
            bound ? bound_port(server->fd()) : fiber::common::IoResult<std::uint16_t>(std::unexpected(bound.error()));
    if (!port) {
        delete server;
        port_promise->set_value(0);
        server_promise->set_value(nullptr);
        co_return;
    }
    port_promise->set_value(*port);
    server_promise->set_value(server);
    fiber::async::spawn(*loop, [server]() { return server->serve(); });
}

fiber::access_server::ProjectConfig project_config(std::uint16_t port) {
    fiber::access_server::RouteConfig route;
    route.path = "/proxy";
    route.addresses = {std::optional<std::string>("127.0.0.1:" + std::to_string(port))};
    route.timeout_millis = 5000;
    route.websocket_timeout_millis = 5000;
    route.max_client_body_size = 64 * 1024;
    route.max_proxy_body_size = 64 * 1024;

    fiber::access_server::ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<fiber::access_server::HostConfigEntry>{fiber::access_server::HostConfigEntry{
            .pattern = "api.example.com",
            .strategy = fiber::access_server::HostStrategyConfig{},
    }};
    config.routes = std::vector<std::optional<fiber::access_server::RouteConfig>>{std::move(route)};
    return config;
}

std::string http_requests(std::uint64_t count) {
    std::string result;
    constexpr std::string_view kKeepAlive =
            "GET /proxy HTTP/1.1\r\nHost: api.example.com\r\nConnection: keep-alive\r\n\r\n";
    constexpr std::string_view kClose = "GET /proxy HTTP/1.1\r\nHost: api.example.com\r\nConnection: close\r\n\r\n";
    result.reserve(static_cast<std::size_t>(count) * kKeepAlive.size());
    for (std::uint64_t request = 0; request < count; ++request) {
        result.append(request + 1 == count ? kClose : kKeepAlive);
    }
    return result;
}

constexpr std::string_view websocket_request() noexcept {
    return "GET /proxy HTTP/1.1\r\n"
           "Host: api.example.com\r\n"
           "Connection: upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
           "client-frame";
}

bool write_all(int fd, std::string_view input) noexcept {
    std::size_t offset = 0;
    while (offset < input.size()) {
        const ssize_t written = ::send(fd, input.data() + offset, input.size() - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

std::optional<std::string> request_loopback(std::uint16_t port, std::string_view input) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    timeval timeout{.tv_sec = 10};
    (void) ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void) ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || !write_all(fd, input)) {
        ::close(fd);
        return std::nullopt;
    }

    std::string output;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            ::close(fd);
            return std::nullopt;
        }
        break;
    }
    ::close(fd);
    return output;
}

std::size_t count_substring(std::string_view input, std::string_view needle) noexcept {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = input.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

struct HttpResult {
    Distribution distribution;
    AllocationMeasurement allocation;
    double response_bytes_per_request = 0;
    bool success = false;
};

HttpResult measure_http(std::uint16_t port, std::uint64_t requests_per_sample) {
    const std::string input = http_requests(requests_per_sample);
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
    std::uint64_t response_bytes = 0;
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        auto output = request_loopback(port, input);
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
        if (!output || count_substring(*output, "HTTP/1.1 200 OK\r\n") != requests_per_sample ||
            count_substring(*output, kHttpResponseBody) != requests_per_sample) {
            return {};
        }
        response_bytes += output->size();
    }

    fiber::access_server::benchmark::begin_allocation_measurement();
    auto allocation_output = request_loopback(port, input);
    const AllocationMeasurement allocation = fiber::access_server::benchmark::finish_allocation_measurement();
    if (!allocation_output || count_substring(*allocation_output, "HTTP/1.1 200 OK\r\n") != requests_per_sample) {
        return {};
    }
    return {
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), requests_per_sample),
            .allocation = allocation,
            .response_bytes_per_request =
                    static_cast<double>(response_bytes) /
                    static_cast<double>(requests_per_sample * fiber::access_server::benchmark::kDefaultSamples),
            .success = true,
    };
}

struct WebSocketResult {
    Distribution distribution;
    double response_bytes_per_session = 0;
    bool success = false;
};

WebSocketResult measure_websocket(std::uint16_t port, std::uint64_t sessions) {
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(static_cast<std::size_t>(sessions));
    std::uint64_t response_bytes = 0;
    for (std::uint64_t session = 0; session < sessions; ++session) {
        const auto started = std::chrono::steady_clock::now();
        auto output = request_loopback(port, websocket_request());
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
        if (!output || output->find("HTTP/1.1 101 Switching Protocols\r\n") == std::string::npos ||
            output->find(kWebSocketServerFrame) == std::string::npos) {
            return {};
        }
        response_bytes += output->size();
    }
    return {
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), 1),
            .response_bytes_per_session = static_cast<double>(response_bytes) / static_cast<double>(sessions),
            .success = true,
    };
}

fiber::async::DetachedTask shutdown(fiber::http::StealableHttp1ConnectionPoolSet *pool,
                                    fiber::http::HttpServer *gateway, fiber::http::HttpServer *upstream,
                                    std::promise<void> *done) {
    gateway->close();
    upstream->close();
    co_await pool->shutdown_async();
    done->set_value();
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t http_requests_per_sample = kDefaultHttpRequestsPerSample;
    std::uint64_t websocket_sessions = kDefaultWebSocketSessions;
    if ((argc >= 2 && !fiber::access_server::benchmark::parse_positive(argv[1], http_requests_per_sample)) ||
        (argc >= 3 && !fiber::access_server::benchmark::parse_positive(argv[2], websocket_sessions)) || argc > 3) {
        std::fprintf(stderr, "usage: %s [http-requests-per-sample] [websocket-sessions]\n", argv[0]);
        return 2;
    }

    SilentLogging logging;
    if (!logging.initialized()) {
        std::fprintf(stderr, "failed to disable benchmark logging\n");
        return 1;
    }

    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet pool(group);
    if (!pool.init()) {
        std::fprintf(stderr, "failed to initialize proxy connection pool\n");
        return 1;
    }
    group.start();

    UpstreamState upstream_state;
    std::promise<std::uint16_t> upstream_port_promise;
    std::promise<fiber::http::HttpServer *> upstream_server_promise;
    auto upstream_port = upstream_port_promise.get_future();
    auto upstream_server = upstream_server_promise.get_future();
    fiber::http::HttpHandler upstream_handler = [&upstream_state](fiber::http::HttpExchange &exchange) {
        return serve_upstream(exchange, &upstream_state);
    };
    fiber::async::spawn(group.at(0), [&]() {
        return start_server(&group.at(0), std::move(upstream_handler), &upstream_port_promise,
                            &upstream_server_promise);
    });
    fiber::http::HttpServer *upstream = upstream_server.get();
    const std::uint16_t upstream_bound_port = upstream_port.get();
    if (upstream == nullptr || upstream_bound_port == 0) {
        group.stop();
        group.join();
        std::fprintf(stderr, "failed to bind loopback upstream\n");
        return 1;
    }

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("benchmark", project_config(upstream_bound_port));
    if (!published) {
        upstream->close();
        group.stop();
        group.join();
        delete upstream;
        std::fprintf(stderr, "failed to compile proxy benchmark route\n");
        return 1;
    }
    fiber::access_server::ProxyExecutor executor(pool);
    fiber::access_server::AccessRequestHandler access_handler(store.snapshot_provider(), {}, {}, executor.adapter());
    fiber::access_server::ClientMetadataResolver metadata_resolver;

    std::promise<std::uint16_t> gateway_port_promise;
    std::promise<fiber::http::HttpServer *> gateway_server_promise;
    auto gateway_port = gateway_port_promise.get_future();
    auto gateway_server = gateway_server_promise.get_future();
    fiber::http::HttpHandler gateway_handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::access_server::AccessRequestTelemetry telemetry(exchange, nullptr, nullptr, nullptr, &metadata_resolver);
        co_await access_handler.handle(exchange, telemetry);
    };
    fiber::async::spawn(group.at(0), [&]() {
        return start_server(&group.at(0), std::move(gateway_handler), &gateway_port_promise, &gateway_server_promise);
    });
    fiber::http::HttpServer *gateway = gateway_server.get();
    const std::uint16_t gateway_bound_port = gateway_port.get();
    if (gateway == nullptr || gateway_bound_port == 0) {
        upstream->close();
        group.stop();
        group.join();
        delete upstream;
        std::fprintf(stderr, "failed to bind loopback gateway\n");
        return 1;
    }

    const HttpResult http = measure_http(gateway_bound_port, http_requests_per_sample);
    const std::uint64_t http_request_count = upstream_state.requests.load(std::memory_order_relaxed);
    const std::uint64_t http_connection_count = upstream_state.connections.load(std::memory_order_relaxed);
    const double pool_hit_ratio =
            http_request_count == 0
                    ? 0.0
                    : static_cast<double>(http_request_count - std::min(http_request_count, http_connection_count)) /
                              static_cast<double>(http_request_count);
    const WebSocketResult websocket = measure_websocket(gateway_bound_port, websocket_sessions);

    std::promise<void> shutdown_promise;
    auto shutdown_done = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, gateway, upstream, &shutdown_promise); });
    const bool shutdown_completed = shutdown_done.wait_for(10s) == std::future_status::ready;
    group.stop();
    group.join();
    delete gateway;
    delete upstream;

    if (!http.success || !websocket.success || !shutdown_completed ||
        upstream_state.websocket_sessions.load(std::memory_order_relaxed) != websocket_sessions) {
        std::fprintf(stderr, "loopback proxy benchmark failed\n");
        return 1;
    }
    std::printf("case,operations,p50_ns_per_operation,p95_ns_per_operation,p99_ns_per_operation,"
                "operations_per_second,allocations_per_operation,allocated_bytes_per_operation,"
                "response_bytes_per_operation,pool_hit_ratio\n");
    std::printf("http_proxy,%llu,%.2f,%.2f,%.2f,%.0f,%.4f,%.2f,%.2f,%.6f\n",
                static_cast<unsigned long long>(http_requests_per_sample), http.distribution.p50_ns_per_operation,
                http.distribution.p95_ns_per_operation, http.distribution.p99_ns_per_operation,
                http.distribution.operations_per_second,
                static_cast<double>(http.allocation.allocations) / static_cast<double>(http_requests_per_sample),
                static_cast<double>(http.allocation.bytes) / static_cast<double>(http_requests_per_sample),
                http.response_bytes_per_request, pool_hit_ratio);
    std::printf("websocket_tunnel,%llu,%.2f,%.2f,%.2f,%.0f,NA,NA,%.2f,NA\n",
                static_cast<unsigned long long>(websocket_sessions), websocket.distribution.p50_ns_per_operation,
                websocket.distribution.p95_ns_per_operation, websocket.distribution.p99_ns_per_operation,
                websocket.distribution.operations_per_second, websocket.response_bytes_per_session);
    std::fprintf(stderr,
                 "samples=%zu upstream_http_requests=%llu upstream_http_connections=%llu websocket_sessions=%llu\n",
                 fiber::access_server::benchmark::kDefaultSamples, static_cast<unsigned long long>(http_request_count),
                 static_cast<unsigned long long>(http_connection_count),
                 static_cast<unsigned long long>(websocket_sessions));
    return 0;
}
