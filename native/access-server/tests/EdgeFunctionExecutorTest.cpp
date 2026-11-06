#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/common/util/Crc32.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1Connection.h>
#include <fiber/http/HttpExchange.h>
#include "HttpTransportStub.h"
#include "config/AccessConfig.h"
#include "execution/AccessRequestHandler.h"
#include "observability/AccessRequestTelemetry.h"
#include "routing/EdgeFunctionLibrary.h"
#include "routing/EdgeFunctionsSnapshot.h"
#include "runtime/RouteConfigStore.h"

namespace {

using namespace std::chrono_literals;

using fiber::access_server::AccessRequestHandler;
using fiber::access_server::AccessRequestHandlerOptions;
using fiber::access_server::BodyType;
using fiber::access_server::ClientMetadataMode;
using fiber::access_server::ClientMetadataResolver;
using fiber::access_server::ClientMetadataResolverOptions;
using fiber::access_server::EdgeFunctionIndexEntry;
using fiber::access_server::EdgeFunctionLibrary;
using fiber::access_server::EdgeFunctionMountConfig;
using fiber::access_server::EdgeFunctionProgram;
using fiber::access_server::EdgeFunctionProjectsSelector;
using fiber::access_server::EdgeFunctionsState;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::ProjectConfig;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteConfigStore;
using fiber::access_server::RouteType;

// ---- request rig (same shape as AccessRequestHandlerTest) ----

class RecordingTransport final : public fiber::test::HttpTransportStub {
public:
    RecordingTransport(fiber::event::EventLoop &loop, std::string input, std::string &output,
                       fiber::net::SocketAddress remote_addr = {}) :
        loop_(loop), input_(std::move(input)), output_(output), remote_addr_(std::move(remote_addr)) {}

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> readv(std::size_t size, fiber::mem::IoBufChain &out,
                                                                   std::chrono::milliseconds) override {
        if (input_offset_ == input_.size()) {
            co_return static_cast<std::size_t>(0);
        }
        const std::size_t count = std::min(size, input_.size() - input_offset_);
        if (count == 0) {
            co_return static_cast<std::size_t>(0);
        }
        auto buffer = fiber::mem::IoBuf::allocate(count);
        if (!buffer) {
            co_return std::unexpected(fiber::common::IoErr::NoMem);
        }
        std::memcpy(buffer.writable_data(), input_.data() + input_offset_, count);
        buffer.commit(count);
        if (!out.append(std::move(buffer))) {
            co_return std::unexpected(fiber::common::IoErr::NoMem);
        }
        input_offset_ += count;
        co_return count;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> writev(fiber::mem::IoBufChain &buffers,
                                                                    std::chrono::milliseconds) override {
        std::array<iovec, 16> iov{};
        const int count = buffers.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        std::size_t size = 0;
        for (int i = 0; i < count; ++i) {
            const auto &entry = iov[static_cast<std::size_t>(i)];
            output_.append(static_cast<const char *>(entry.iov_base), entry.iov_len);
            size += entry.iov_len;
        }
        buffers.consume_and_compact(size);
        co_return size;
    }

    void close() override { closed_ = true; }
    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return {}; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_; }

private:
    fiber::event::EventLoop &loop_;
    std::string input_;
    std::string &output_;
    fiber::net::SocketAddress remote_addr_{};
    std::size_t input_offset_ = 0;
    bool closed_ = false;
};

fiber::async::DetachedTask run_request_on_loop(fiber::event::EventLoop *loop, const RouteConfigStore *store,
                                               std::string request, std::string *output, std::promise<void> *done,
                                               fiber::net::SocketAddress remote_addr = {}) {
    auto transport = std::make_unique<RecordingTransport>(*loop, std::move(request), *output, std::move(remote_addr));
    fiber::access_server::AccessRequestHandler access_handler(store->snapshot_provider(), {},
                                                              AccessRequestHandlerOptions{}, {});
    ClientMetadataResolver client_metadata_resolver(ClientMetadataResolverOptions{
            .mode = ClientMetadataMode::LegacyHeaders,
    });
    fiber::http::HttpHandler handler = [&access_handler, &client_metadata_resolver](
                                               fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::access_server::AccessRequestTelemetry telemetry(exchange, nullptr, nullptr, &client_metadata_resolver);
        co_await access_handler.handle(exchange, telemetry);
    };
    fiber::http::Http1Connection connection(std::move(transport), handler, {});
    co_await connection.run();
    done->set_value();
    co_return;
}

std::string run_request(const RouteConfigStore &store, std::string request,
                        fiber::net::SocketAddress remote_addr = {}) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::string output;
    std::promise<void> done;
    auto completed = done.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_request_on_loop(&group.at(0), &store, std::move(request), &output, &done, std::move(remote_addr));
    });

    EXPECT_EQ(completed.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    return output;
}

// ---- project + edge-function state builders ----

void publish_response_route(RouteConfigStore &store) {
    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{HostConfigEntry{
            .pattern = "api.example.com",
            .strategy = HostStrategyConfig{},
    }};
    RouteConfig route;
    route.path = "/anything";
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Template,
            .content = "route-body",
    };
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    auto published = store.apply("orders", config);
    EXPECT_TRUE(published) << (published ? "" : published.error().message);
}

EdgeFunctionMountConfig mounted_everywhere(std::int32_t priority) {
    return EdgeFunctionMountConfig{
            .projects = EdgeFunctionProjectsSelector{.include = {"*"}},
            .priority = priority,
    };
}

EdgeFunctionMountConfig mounted_everywhere(std::int32_t priority, std::int32_t ratio) {
    return EdgeFunctionMountConfig{
            .projects = EdgeFunctionProjectsSelector{.include = {"*"}},
            .priority = priority,
            .ratio = ratio,
    };
}

// Same sticky-sampling bucket as the executor: crc32 over the raw peer IP
// bytes % 10000. Deterministic, so fixture addresses with known buckets can
// be brute-forced at test time instead of hard-coded.
std::uint32_t sampling_bucket(const fiber::net::IpAddress &peer) {
    return fiber::util::Crc32::compute(std::span<const std::uint8_t>{peer.data(), peer.byte_size()}) %
           fiber::access_server::kEdgeFunctionFullRatio;
}

fiber::net::SocketAddress address_with_bucket(std::uint32_t wanted) {
    for (int a = 0; a < 256; ++a) {
        for (int b = 0; b < 256; ++b) {
            fiber::net::IpAddress ip;
            if (!fiber::net::IpAddress::parse("198.51." + std::to_string(a) + "." + std::to_string(b), ip)) {
                continue;
            }
            if (sampling_bucket(ip) == wanted) {
                return fiber::net::SocketAddress(ip, 52341);
            }
        }
    }
    return {};
}

EdgeFunctionMountConfig mounted_on(std::string project_glob, std::int32_t priority) {
    return EdgeFunctionMountConfig{
            .projects = EdgeFunctionProjectsSelector{.include = {std::move(project_glob)}},
            .priority = priority,
    };
}

void add_function(EdgeFunctionsState &state, EdgeFunctionLibrary &library, std::string name, std::string_view source,
                  std::vector<EdgeFunctionMountConfig> mounts) {
    auto compiled = library.compile(source);
    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return;
    }
    state.total_script_bytes += source.size();
    state.programs.push_back(std::make_shared<EdgeFunctionProgram>(std::move(*compiled)));
    EdgeFunctionIndexEntry entry;
    entry.name = std::move(name);
    entry.mounts = std::move(mounts);
    state.index.functions.push_back(std::move(entry));
}

std::string request() {
    return "GET /anything HTTP/1.1\r\n"
           "Host: api.example.com\r\n"
           "Connection: close\r\n"
           "\r\n";
}

std::string_view response_body(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return {};
    }
    return response.substr(separator + 4);
}

// ---- end-to-end: handler hook → mount table → executor ----

TEST(EdgeFunctionExecutor, ShortCircuitBeatsLaterFunctionsAndRouting) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "later", R"(resp.send(200, "later-function");)", {mounted_everywhere(2)});
    add_function(state, library, "blocker", R"(resp.send(403, "blocked-by-blocker");)", {mounted_everywhere(1)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request());
    EXPECT_NE(response.find(" 403 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("blocked-by-blocker"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, RatioSamplingExecutesOnlySampledPeers) {
    // ratio 1: only peers whose sticky bucket is exactly 0 run the function;
    // everyone else flows straight to the route.
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "canary", R"(resp.send(403, "canary-blocked");)", {mounted_everywhere(0, 1)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string sampled = run_request(store, request(), address_with_bucket(0));
    EXPECT_NE(sampled.find(" 403 "), std::string::npos) << sampled;
    EXPECT_NE(response_body(sampled).find("canary-blocked"), std::string::npos) << sampled;

    const std::string skipped = run_request(store, request(), address_with_bucket(9999));
    EXPECT_NE(skipped.find(" 200 "), std::string::npos) << skipped;
    EXPECT_NE(response_body(skipped).find("route-body"), std::string::npos) << skipped;
}

TEST(EdgeFunctionExecutor, RatioZeroMountStaysDisabledButChainContinues) {
    // ratio 0 is mounted-but-disabled: the function never executes for any
    // peer, and the chain moves on to the next mounted function.
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "off", R"(resp.send(200, "never-off");)", {mounted_everywhere(1, 0)});
    add_function(state, library, "responder", R"(resp.send(403, "after-disabled");)", {mounted_everywhere(2)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request(), address_with_bucket(0));
    EXPECT_NE(response.find(" 403 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("after-disabled"), std::string::npos) << response;
    EXPECT_EQ(response.find("never-off"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, ExceptionFailsOpenToRouting) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    // ctx.report with a non-string argument raises inside the VM.
    add_function(state, library, "broken", R"(ctx.report(1, "x"); true)", {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request());
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("route-body"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, ExceptionContinuesChainToNextFunction) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "broken", R"(ctx.report(1, "x"); true)", {mounted_everywhere(1)});
    add_function(state, library, "responder", R"(resp.send(403, "second-function");)", {mounted_everywhere(2)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request());
    EXPECT_NE(response.find(" 403 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("second-function"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, ValueWithoutResponseContinues) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "observer", R"(return true;)", {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request());
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("route-body"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, UnmountedProjectBypassesFunctions) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "other", R"(resp.send(403, "never");)", {mounted_on("not-orders", 0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request());
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("route-body"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, EmptyIndexUnmountsAfterSet) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "blocker", R"(resp.send(403, "blocked");)", {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));
    EXPECT_NE(run_request(store, request()).find(" 403 "), std::string::npos);

    store.set_edge_functions(std::make_shared<EdgeFunctionsState>());
    const std::string response = run_request(store, request());
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("route-body"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, FunctionSeesProjectAndHostContext) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    // Responds only when the mounted project and the request host match the
    // executor-provided context; otherwise the request flows to the route.
    add_function(state, library, "guard",
                 R"(if ($ctx.project == "orders" && $ctx.host == "api.example.com") {
    resp.send(403, "ctx-match");
}
return true;)",
                 {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request());
    EXPECT_NE(response.find(" 403 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("ctx-match"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, FunctionReadsHeaderAndQueryConstants) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    // $header/$query/$cookie constants resolve dynamically against the request
    // (RouteScriptExtension). Constant keys use the normalized spelling (lowercase,
    // '_' for '-'): $header.x_edge_token matches the x-edge-token request header.
    add_function(state, library, "guard",
                 R"(if ($header.x_edge_token == "s3cret" && $query.mode == "debug") {
    resp.send(403, "header-query-match");
}
return true;)",
                 {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string blocked = "GET /anything?mode=debug HTTP/1.1\r\n"
                                "Host: api.example.com\r\n"
                                "x-edge-token: s3cret\r\n"
                                "Connection: close\r\n"
                                "\r\n";
    const std::string blocked_response = run_request(store, blocked);
    EXPECT_NE(blocked_response.find(" 403 "), std::string::npos) << blocked_response;
    EXPECT_NE(response_body(blocked_response).find("header-query-match"), std::string::npos) << blocked_response;

    // Without the header the function must not fire and the route answers.
    const std::string allowed = "GET /anything?mode=debug HTTP/1.1\r\n"
                                "Host: api.example.com\r\n"
                                "Connection: close\r\n"
                                "\r\n";
    const std::string allowed_response = run_request(store, allowed);
    EXPECT_NE(allowed_response.find(" 200 "), std::string::npos) << allowed_response;
    EXPECT_NE(response_body(allowed_response).find("route-body"), std::string::npos) << allowed_response;
}

} // namespace
