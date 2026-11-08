#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
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
#include "config/AccessConfigLimits.h"
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
                                               fiber::net::SocketAddress remote_addr = {},
                                               AccessRequestHandlerOptions handler_options = {},
                                               fiber::access_server::AccessProxyAdapter proxy_adapter = {}) {
    auto transport = std::make_unique<RecordingTransport>(*loop, std::move(request), *output, std::move(remote_addr));
    fiber::access_server::AccessRequestHandler access_handler(store->snapshot_provider(), {}, handler_options,
                                                              proxy_adapter);
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

std::string run_request(const RouteConfigStore &store, std::string request, fiber::net::SocketAddress remote_addr = {},
                        AccessRequestHandlerOptions handler_options = {},
                        fiber::access_server::AccessProxyAdapter proxy_adapter = {}) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::string output;
    std::promise<void> done;
    auto completed = done.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_request_on_loop(&group.at(0), &store, std::move(request), &output, &done, std::move(remote_addr),
                                   handler_options, proxy_adapter);
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

// How a test function is mounted; add_function copies it onto the index entry.
struct TestMount {
    EdgeFunctionProjectsSelector projects;
    std::int32_t priority = 0;
    std::int32_t ratio = fiber::access_server::kEdgeFunctionFullRatio;
    std::string replaces;
};

void apply_mount(EdgeFunctionIndexEntry &entry, TestMount mount) {
    entry.projects = std::move(mount.projects);
    entry.priority = mount.priority;
    entry.ratio = mount.ratio;
    entry.replaces = std::move(mount.replaces);
}

TestMount mounted_everywhere(std::int32_t priority) {
    return TestMount{
            .projects = EdgeFunctionProjectsSelector{.include = {"*"}},
            .priority = priority,
    };
}

TestMount mounted_everywhere(std::int32_t priority, std::int32_t ratio) {
    return TestMount{
            .projects = EdgeFunctionProjectsSelector{.include = {"*"}},
            .priority = priority,
            .ratio = ratio,
    };
}

TestMount canary_of(std::string target, std::int32_t priority, std::int32_t ratio) {
    return TestMount{
            .projects = EdgeFunctionProjectsSelector{.include = {"*"}},
            .priority = priority,
            .ratio = ratio,
            .replaces = std::move(target),
    };
}

// Same sticky-sampling bucket as the executor: crc32 over the raw client IP
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

TestMount mounted_on(std::string project_glob, std::int32_t priority) {
    return TestMount{
            .projects = EdgeFunctionProjectsSelector{.include = {std::move(project_glob)}},
            .priority = priority,
    };
}

void add_function(EdgeFunctionsState &state, EdgeFunctionLibrary &library, std::string name, std::string_view source,
                  TestMount mount) {
    auto compiled = library.compile(source);
    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return;
    }
    state.total_script_bytes += source.size();
    state.programs.push_back(std::make_shared<EdgeFunctionProgram>(std::move(*compiled)));
    EdgeFunctionIndexEntry entry;
    entry.name = std::move(name);
    apply_mount(entry, std::move(mount));
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

TEST(EdgeFunctionExecutor, SamplingFollowsTheResolvedClientAddress) {
    // Behind a proxy the socket peer is an intermediary shared by many clients,
    // so the draw must follow the resolved client address. The rig resolves
    // X-Real-Ip (legacy headers); without one the peer stands in, which is what
    // RatioSamplingExecutesOnlySampledPeers covers.
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "canary", R"(resp.send(403, "canary-blocked");)", {mounted_everywhere(0, 1)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const fiber::net::SocketAddress hit = address_with_bucket(0);
    const fiber::net::SocketAddress miss = address_with_bucket(9999);
    const auto from_client = [](const fiber::net::SocketAddress &client) {
        return "GET /anything HTTP/1.1\r\n"
               "Host: api.example.com\r\n"
               "X-Real-Ip: " +
               client.ip().to_string() +
               "\r\n"
               "Connection: close\r\n"
               "\r\n";
    };

    const std::string sampled = run_request(store, from_client(hit), miss);
    EXPECT_NE(response_body(sampled).find("canary-blocked"), std::string::npos) << sampled;

    const std::string skipped = run_request(store, from_client(miss), hit);
    EXPECT_NE(response_body(skipped).find("route-body"), std::string::npos) << skipped;
}

TEST(EdgeFunctionExecutor, ReplacesRunsExactlyOneVersionPerClient) {
    // Each version answers, so whichever the chain reaches first would win if
    // the other's window leaked. Running both priority orders proves the
    // windows exclude each other in both directions.
    for (const auto &[stable_priority, canary_priority]: {std::pair{1, 2}, std::pair{2, 1}}) {
        RouteConfigStore store;
        publish_response_route(store);
        EdgeFunctionLibrary library;
        EdgeFunctionsState state;
        add_function(state, library, "auth-v1", R"(resp.send(200, "stable-version");)",
                     {mounted_everywhere(stable_priority)});
        add_function(state, library, "auth-v2", R"(resp.send(200, "canary-version");)",
                     {canary_of("auth-v1", canary_priority, 300)});
        store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

        for (const std::uint32_t bucket: {0U, 299U}) {
            const std::string response = run_request(store, request(), address_with_bucket(bucket));
            EXPECT_NE(response_body(response).find("canary-version"), std::string::npos)
                    << "bucket " << bucket << ": " << response;
        }
        for (const std::uint32_t bucket: {300U, 9999U}) {
            const std::string response = run_request(store, request(), address_with_bucket(bucket));
            EXPECT_NE(response_body(response).find("stable-version"), std::string::npos)
                    << "bucket " << bucket << ": " << response;
        }
    }
}

TEST(EdgeFunctionExecutor, FullRatioCanaryTakesEveryClient) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "auth-v1", R"(resp.send(200, "stable-version");)", {mounted_everywhere(1)});
    add_function(state, library, "auth-v2", R"(resp.send(200, "canary-version");)", {canary_of("auth-v1", 2, 10000)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request(), address_with_bucket(9999));
    EXPECT_NE(response_body(response).find("canary-version"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, CanaryMissRunsNothingWhereTheTargetIsNotMounted) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "auth-v1", R"(resp.send(200, "stable-version");)", {mounted_on("not-orders", 0)});
    add_function(state, library, "auth-v2", R"(resp.send(200, "canary-version");)", {canary_of("auth-v1", 0, 300)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string canary = run_request(store, request(), address_with_bucket(0));
    EXPECT_NE(response_body(canary).find("canary-version"), std::string::npos) << canary;

    const std::string miss = run_request(store, request(), address_with_bucket(9999));
    EXPECT_NE(response_body(miss).find("route-body"), std::string::npos) << miss;
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

TEST(EdgeFunctionExecutor, FailCloseRejectsWhenFunctionErrorsBeforeResponse) {
    // Strict posture (ACCESS_SERVER_EDGE_FUNCTION_FAIL_CLOSE=true): the first
    // function error rejects with 503 instead of failing open into routing.
    AccessRequestHandlerOptions options;
    options.edge_function_fail_close = true;

    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "broken", R"(ctx.report(1, "x"); true)", {mounted_everywhere(1)});
    add_function(state, library, "responder", R"(resp.send(403, "second-function");)", {mounted_everywhere(2)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, request(), {}, options);
    EXPECT_NE(response.find(" 503 "), std::string::npos) << response;
    EXPECT_NE(response.find("EDGE_FUNCTION_FAIL_CLOSE"), std::string::npos) << response;
    // The chain stopped at the error: the later function never ran.
    EXPECT_EQ(response.find("second-function"), std::string::npos) << response;
    EXPECT_EQ(response_body(response).find("route-body"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, FailCloseKeepsCommittedResponseAndDefaultStaysFailOpen) {
    RouteConfigStore store;
    publish_response_route(store);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    // resp.send commits the response; the exception afterwards cannot un-send
    // it, so even strict mode keeps the 403 the function already produced.
    add_function(state, library, "sent-then-broken", R"(resp.send(403, "already-sent"); ctx.report(1, "x"))",
                 {mounted_everywhere(1)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    AccessRequestHandlerOptions strict;
    strict.edge_function_fail_close = true;
    const std::string committed = run_request(store, request(), {}, strict);
    EXPECT_NE(committed.find(" 403 "), std::string::npos) << committed;
    EXPECT_NE(response_body(committed).find("already-sent"), std::string::npos) << committed;

    // Default (flag off) keeps the documented fail-open behavior.
    const std::string fail_open = run_request(store, request());
    EXPECT_NE(fail_open.find(" 403 "), std::string::npos) << fail_open;
    EXPECT_NE(response_body(fail_open).find("already-sent"), std::string::npos) << fail_open;
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

// ---- body-phase end-to-end (design §7) ----

void publish_proxy_route(RouteConfigStore &store, bool edge_function_body) {
    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{HostConfigEntry{
            .pattern = "api.example.com",
            .strategy = HostStrategyConfig{},
    }};
    RouteConfig route;
    route.path = "/anything";
    route.service = "orders/gray";
    route.edge_function_body = edge_function_body;
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    auto published = store.apply("orders", config);
    EXPECT_TRUE(published) << (published ? "" : published.error().message);
}

void add_body_function(EdgeFunctionsState &state, EdgeFunctionLibrary &library, std::string name,
                       std::string_view source, TestMount mount) {
    auto compiled = library.compile(source, fiber::access_server::EdgeFunctionPhase::RequestBody);
    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return;
    }
    state.total_script_bytes += source.size();
    state.programs.push_back(std::make_shared<EdgeFunctionProgram>(std::move(*compiled)));
    EdgeFunctionIndexEntry entry;
    entry.name = std::move(name);
    entry.phase = fiber::access_server::EdgeFunctionPhase::RequestBody;
    apply_mount(entry, std::move(mount));
    state.index.functions.push_back(std::move(entry));
}

// Mirrors ProxyRequestBodyReader's splice contract: drain the handler's
// prefetched chain first, then keep reading the exchange until the body
// completes. received_body must equal the full client body either way.
struct CapturedBodyProxy {
    std::string received_body;
};

fiber::async::Task<fiber::access_server::Result<void>>
capture_spliced_body_proxy(void *context, fiber::http::HttpExchange &exchange,
                           const fiber::access_server::CompiledProxyRoute &,
                           fiber::access_server::ProxyExecutionInput input,
                           fiber::access_server::AccessRequestTelemetry &telemetry) noexcept {
    auto &capture = *static_cast<CapturedBodyProxy *>(context);
    capture.received_body.clear();
    if (input.prefetched_body != nullptr) {
        while (input.prefetched_body->readable_bytes() != 0) {
            fiber::mem::IoBufChain prefix;
            if (!input.prefetched_body->take_prefix(
                        std::min<std::size_t>(64U * 1024U, input.prefetched_body->readable_bytes()), prefix)) {
                co_return std::unexpected(fiber::access_server::Err::from_error(fiber::common::IoErr::NoMem));
            }
            while (const fiber::mem::IoBuf *part = prefix.first_readable()) {
                capture.received_body.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
                prefix.consume_and_compact(part->readable());
            }
        }
    }
    if (!exchange.request_body_spec().is_none()) {
        for (;;) {
            auto body = co_await exchange.read_body(64U * 1024U);
            if (!body) {
                co_return std::unexpected(fiber::access_server::Err::from_error(body.error()));
            }
            const bool complete = body->complete();
            while (const fiber::mem::IoBuf *part = body->first_readable()) {
                capture.received_body.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
                body->consume_and_compact(part->readable());
            }
            if (complete) {
                break;
            }
        }
    }
    const std::string_view echoed = capture.received_body;
    fiber::http::HttpHeaders &response_headers = telemetry.response_headers();
    if (!response_headers.set("X-Proxy-Fixture", "captured") || !telemetry.finalize_response_headers()) {
        co_return std::unexpected(fiber::access_server::Err::from_error(fiber::common::IoErr::NoMem));
    }
    auto sent_header = co_await telemetry.response_writer().send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &response_headers,
            .body = fiber::http::HttpBodySpec::ContentLength(echoed.size()),
            .connection_mode = fiber::http::ResponseConnectionMode::Auto,
            .end_stream = false,
    });
    if (!sent_header) {
        co_return std::unexpected(fiber::access_server::Err::from_error(sent_header.error()));
    }
    if (echoed.empty()) {
        co_return fiber::access_server::Result<void>{};
    }
    auto sent_body = co_await telemetry.response_writer().write_all(
            reinterpret_cast<const std::uint8_t *>(echoed.data()), echoed.size(), true);
    if (!sent_body) {
        co_return std::unexpected(fiber::access_server::Err::from_error(sent_body.error()));
    }
    co_return fiber::access_server::Result<void>{};
}

fiber::access_server::AccessProxyAdapter body_proxy_adapter(CapturedBodyProxy &capture) {
    return fiber::access_server::AccessProxyAdapter{
            .context = &capture,
            .execute = capture_spliced_body_proxy,
    };
}

// The dialect has no infix `contains` inside an if-condition; textual body
// scans use strings.match (design §3.4 sample shape).
const char *kBodyGuardScript = R"(if (strings.match($req.body, "forbidden")) {
    resp.send(403, "body-blocked");
}
return true;)";

constexpr std::size_t kBodyPhaseCap = fiber::access_server::kAccessConfigLimits.edge_functions.max_body_phase_bytes;

std::string post_json(std::string_view body) {
    std::string request = "POST /anything HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request.append(body);
    return request;
}

TEST(EdgeFunctionExecutor, BodyFunctionSeesBodyAndReplaysItToTheProxy) {
    RouteConfigStore store;
    publish_proxy_route(store, true);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_body_function(state, library, "body-guard", kBodyGuardScript, {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    CapturedBodyProxy capture;
    // A clean body reaches the function (empty $req.body baseline works) and
    // is replayed verbatim to the proxy.
    const std::string clean = run_request(store, post_json("{\"ok\":true}"), {}, {}, body_proxy_adapter(capture));
    EXPECT_NE(clean.find(" 200 "), std::string::npos) << clean;
    EXPECT_EQ(response_body(clean), "{\"ok\":true}") << clean;
    EXPECT_EQ(capture.received_body, "{\"ok\":true}");

    // The forbidden marker is visible through $req.body and short-circuits
    // before the proxy runs.
    const std::string blocked =
            run_request(store, post_json("{\"v\":\"forbidden\"}"), {}, {}, body_proxy_adapter(capture));
    EXPECT_NE(blocked.find(" 403 "), std::string::npos) << blocked;
    EXPECT_NE(response_body(blocked).find("body-blocked"), std::string::npos) << blocked;
}

TEST(EdgeFunctionExecutor, BodyFunctionSkippedWithoutRouteOptIn) {
    RouteConfigStore store;
    publish_proxy_route(store, false);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_body_function(state, library, "body-guard", kBodyGuardScript, {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    CapturedBodyProxy capture;
    const std::string response = run_request(store, post_json("forbidden"), {}, {}, body_proxy_adapter(capture));
    // The marker is present but the route never opted in, so the function
    // does not run and the body still flows to the proxy untouched.
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_EQ(capture.received_body, "forbidden");
}

TEST(EdgeFunctionExecutor, BodyFunctionSkippedForNonWhitelistedContentType) {
    RouteConfigStore store;
    publish_proxy_route(store, true);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_body_function(state, library, "body-guard", kBodyGuardScript, {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string request = "POST /anything HTTP/1.1\r\n"
                                "Host: api.example.com\r\n"
                                "Content-Type: application/octet-stream\r\n"
                                "Content-Length: 9\r\n"
                                "Connection: close\r\n\r\n"
                                "forbidden";
    CapturedBodyProxy capture;
    const std::string response = run_request(store, request, {}, {}, body_proxy_adapter(capture));
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_EQ(capture.received_body, "forbidden");
}

TEST(EdgeFunctionExecutor, WhitelistStripsParametersAndIgnoresCase) {
    RouteConfigStore store;
    publish_proxy_route(store, true);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_body_function(state, library, "body-guard", kBodyGuardScript, {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string request = "POST /anything HTTP/1.1\r\n"
                                "Host: api.example.com\r\n"
                                "Content-Type: APPLICATION/JSON; charset=utf-8\r\n"
                                "Content-Length: 9\r\n"
                                "Connection: close\r\n\r\n"
                                "forbidden";
    const std::string response = run_request(store, request);
    // Parameters are stripped and the comparison is case-insensitive, so the
    // function still sees the body and blocks.
    EXPECT_NE(response.find(" 403 "), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, BodyFunctionSkippedWhenContentLengthExceedsCap) {
    RouteConfigStore store;
    publish_proxy_route(store, true);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_body_function(state, library, "body-guard", kBodyGuardScript, {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    // One byte over max_body_phase_bytes (1 MiB): declared length skips the
    // chain without touching the exchange, and the proxy still receives the
    // entire body.
    std::string body(kBodyPhaseCap + 1U, 'x');
    body.append("forbidden");
    CapturedBodyProxy capture;
    const std::string response = run_request(store, post_json(body), {}, {}, body_proxy_adapter(capture));
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_EQ(capture.received_body, body);
}

TEST(EdgeFunctionExecutor, StreamingOverflowSkipsFunctionsAndSplicesWholeBody) {
    RouteConfigStore store;
    publish_proxy_route(store, true);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_body_function(state, library, "body-guard", kBodyGuardScript, {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    // Chunked body over the cap: the prefetch keeps a bounded prefix, the
    // functions are skipped, and the prefix plus the live tail still reach
    // the proxy whole — never a full buffer, never a dropped byte.
    const std::size_t cap = kBodyPhaseCap;
    std::string first_chunk(cap + 1U, 'y');
    std::string request = "POST /anything HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Content-Type: application/json\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Connection: close\r\n\r\n";
    request += std::format("{:x}\r\n", first_chunk.size());
    request += first_chunk;
    request += "\r\n5\r\ntail!\r\n0\r\n\r\n";

    CapturedBodyProxy capture;
    const std::string response = run_request(store, request, {}, {}, body_proxy_adapter(capture));
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_EQ(capture.received_body, first_chunk + "tail!");
}

TEST(EdgeFunctionExecutor, BodyFunctionRunsForEmptyBodyWithWhitelistedType) {
    RouteConfigStore store;
    publish_proxy_route(store, true);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_body_function(state, library, "empty-guard", R"(if ($req.body == "") {
    resp.send(200, "empty-body-seen");
}
return true;)",
                      {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, post_json(""));
    EXPECT_NE(response.find(" 200 "), std::string::npos) << response;
    EXPECT_NE(response_body(response).find("empty-body-seen"), std::string::npos) << response;
}

TEST(EdgeFunctionExecutor, HeadFunctionsStillRunBeforeBodyPhase) {
    RouteConfigStore store;
    publish_proxy_route(store, true);
    EdgeFunctionLibrary library;
    EdgeFunctionsState state;
    add_function(state, library, "head-first", R"(resp.send(200, "head-wins");)", {mounted_everywhere(0)});
    add_body_function(state, library, "body-second", R"(resp.send(200, "body-wins");)", {mounted_everywhere(0)});
    store.set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));

    const std::string response = run_request(store, post_json("x"));
    EXPECT_NE(response_body(response).find("head-wins"), std::string::npos) << response;
    EXPECT_EQ(response_body(response).find("body-wins"), std::string::npos) << response;
}

} // namespace
