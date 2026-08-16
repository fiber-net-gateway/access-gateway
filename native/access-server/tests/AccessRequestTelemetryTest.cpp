#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClient.h>
#include <fiber/cat/CatClientConfig.h>
#include <fiber/cat/Transaction.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1Connection.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http_script/ConstPackage.h>
#include <fiber/script/JsValue.h>

#include "HttpTransportStub.h"
#include "execution/ClientMetadata.h"
#include "observability/AccessRequestTelemetry.h"
#include "observability/RequestObservability.h"
#include "observability/ScriptExecutionContext.h"
#include "observability/TracePropagation.h"

namespace {

using namespace std::chrono_literals;

class TelemetryTransport final : public fiber::test::HttpTransportStub {
public:
    TelemetryTransport(fiber::event::EventLoop &loop, std::string input) : loop_(loop), input_(std::move(input)) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read(void *, std::size_t,
                                                                  std::chrono::milliseconds) override {
        co_return static_cast<std::size_t>(0);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read_into(fiber::mem::IoBuf &buffer,
                                                                       std::chrono::milliseconds) override {
        if (input_consumed_) {
            co_return static_cast<std::size_t>(0);
        }
        if (buffer.writable() < input_.size()) {
            co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
        }
        std::memcpy(buffer.writable_data(), input_.data(), input_.size());
        buffer.commit(input_.size());
        input_consumed_ = true;
        co_return input_.size();
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                        std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(const void *, std::size_t size,
                                                                   std::chrono::milliseconds) override {
        co_return size;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(fiber::mem::IoBuf &buffer,
                                                                   std::chrono::milliseconds) override {
        const std::size_t size = buffer.readable();
        buffer.consume(size);
        co_return size;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> writev(fiber::mem::IoBufChain &buffers,
                                                                    std::chrono::milliseconds) override {
        const std::size_t size = buffers.size();
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
    fiber::net::SocketAddress remote_addr_{};
    bool input_consumed_ = false;
    bool closed_ = false;
};

struct ComponentResult {
    bool constants_prepared = false;
    bool trace_bound = false;
    bool context_updated = false;
    bool context_removed = false;
    bool upstream_injected = false;
    bool response_finalized = false;
    bool cat_disabled = false;
    bool provider_disabled = false;
    std::string initial_tenant;
    std::string bound_trace_parent;
    std::string updated_tenant;
    std::string trace_parent;
    std::string upstream_trace_state;
    std::string upstream_trace_id;
    std::string response_trace_id;
    std::string copied_value;
};

std::string read_string_constant(const fiber::http_script::ScriptExchangeCtx &context,
                                 const fiber::http_script::ConstPackage &constants,
                                 fiber::http_script::ConstIndex index) {
    const fiber::script::AbiResult value = context.constant(constants.identity(), index);
    if (!value || !fiber::script::js_value_is_string(value.value())) {
        return {};
    }
    const fiber::script::NativeStr string = fiber::script::js_value_native_string(value.value());
    return std::string(string.data, string.len);
}

fiber::async::DetachedTask run_component_request(fiber::event::EventLoop *loop,
                                                 std::shared_ptr<const fiber::http_script::ConstPackage> constants,
                                                 fiber::cat::CatClient *cat_client, ComponentResult *result,
                                                 std::promise<void> *done) {
    auto transport = std::make_unique<TelemetryTransport>(
            *loop, "GET /telemetry HTTP/1.1\r\n"
                   "Host: api.example.com\r\n"
                   "traceparent: 00-11111111111111111111111111111111-2222222222222222-01\r\n"
                   "tracestate: vendor=opaque,bnrc=aL8nZlUy-1nka5V\r\n"
                   "Connection: close\r\n\r\n");
    fiber::http::HttpHandler handler = [constants = std::move(constants), cat_client,
                                        result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::access_server::ScriptExecutionContext execution(exchange);
        fiber::access_server::ClientMetadataResolver resolver;
        const fiber::access_server::ClientMetadata metadata = resolver.resolve(exchange);
        fiber::access_server::TracePropagation trace(exchange.pool());
        fiber::access_server::RequestObservability observability(exchange, nullptr, cat_client, nullptr, metadata,
                                                                 trace);

        const auto prepared = execution.script_context().prepare_constants(*constants);
        result->constants_prepared = prepared.has_value();
        if (prepared) {
            result->trace_bound = trace.bind_trace_context(execution, *constants).has_value();
        }

        const auto tenant_index = constants->find(fiber::http_script::ConstType::Context, "tenant");
        const auto trace_parent_index = constants->find(fiber::http_script::ConstType::Header, "traceparent");
        result->initial_tenant = read_string_constant(execution.script_context(), *constants, tenant_index);
        result->bound_trace_parent = read_string_constant(execution.script_context(), *constants, trace_parent_index);

        result->context_updated =
                trace.put_trace_context(execution, observability.root_transaction(), "tenant", "green").has_value();
        result->updated_tenant = read_string_constant(execution.script_context(), *constants, tenant_index);
        trace.remove_trace_context(execution, observability.root_transaction(), "tenant");
        const fiber::script::AbiResult removed =
                execution.script_context().constant(constants->identity(), tenant_index);
        result->context_removed = !trace.trace_context("tenant") && removed &&
                                  fiber::script::js_value_type(removed.value()) == fiber::script::JsNodeType::Null;

        (void) trace.put_trace_context(execution, observability.root_transaction(), "region", "east");
        fiber::http::HttpHeaders upstream_headers(execution.pool());
        fiber::access_server::AccessProviderTransaction provider;
        result->upstream_injected =
                trace.inject_upstream_headers(upstream_headers, execution, observability.root_transaction(), provider);
        result->upstream_trace_state.assign(upstream_headers.get("tracestate"));
        result->upstream_trace_id.assign(upstream_headers.get("hi-trace-id"));

        result->trace_parent.assign(trace.trace_parent());
        result->response_finalized = trace.finalize_response_headers(execution.response_headers());
        result->response_trace_id.assign(execution.response_headers().get("hi-trace-id"));
        result->cat_disabled = !observability.root_transaction().valid() && trace.trace_id().empty();
        result->provider_disabled = !observability.start_provider_transaction("disabled").valid();
        result->copied_value.assign(execution.copy_to_request_pool("request-owned"));

        auto sent = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .body = fiber::http::HttpBodySpec::ContentLength(0),
                .end_stream = true,
        });
        (void) sent;
        observability.finish(exchange, metadata, trace.trace_id());
    };
    fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(handler), {});
    co_await connection.run();
    done->set_value();
}

ComponentResult run_component_request(bool wrong_cat_loop = false) {
    fiber::http_script::ConstPackage::Builder builder;
    (void) builder.add_constant(fiber::http_script::ConstType::Context, "tenant");
    (void) builder.add_constant(fiber::http_script::ConstType::Header, "traceparent");
    auto constants = builder.build();

    fiber::event::EventLoopGroup group(wrong_cat_loop ? 2 : 1);
    std::unique_ptr<fiber::cat::CatClient> cat_client;
    if (wrong_cat_loop) {
        fiber::cat::CatClientConfigParams params{
                .app_key = "telemetry-test",
                .hostname = "test-host",
                .ip = "127.0.0.1",
                .thread_group_name = "test",
                .thread_id = "0",
                .thread_name = "worker",
                .bootstrap_collectors =
                        {
                                fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 1),
                        },
        };
        auto config = fiber::cat::CatClientConfig::create(std::move(params));
        if (!config) {
            ADD_FAILURE() << "failed to construct CAT test config";
            return {};
        }
        auto created = fiber::cat::CatClient::create(group.at(0), std::move(*config));
        if (!created) {
            ADD_FAILURE() << "failed to construct CAT test client";
            return {};
        }
        cat_client = std::move(*created);
    }
    group.start();
    ComponentResult result;
    std::promise<void> done;
    auto completed = done.get_future();
    const std::size_t request_worker = wrong_cat_loop ? 1 : 0;
    fiber::async::spawn(group.at(request_worker), [&]() {
        return run_component_request(&group.at(request_worker), std::move(constants), cat_client.get(), &result, &done);
    });
    EXPECT_EQ(completed.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    return result;
}

TEST(AccessRequestTelemetryTest, KeepsFacadeAtBaselineSizeWithoutPolymorphicComponents) {
    EXPECT_EQ(sizeof(fiber::access_server::AccessRequestTelemetry), 864U);
    EXPECT_EQ(sizeof(fiber::access_server::AccessRequestTelemetry),
              sizeof(fiber::access_server::ScriptExecutionContext) + sizeof(fiber::access_server::ClientMetadata) +
                      sizeof(fiber::access_server::TracePropagation) +
                      sizeof(fiber::access_server::RequestObservability));
    EXPECT_FALSE(std::is_polymorphic_v<fiber::access_server::ScriptExecutionContext>);
    EXPECT_FALSE(std::is_polymorphic_v<fiber::access_server::TracePropagation>);
    EXPECT_FALSE(std::is_polymorphic_v<fiber::access_server::RequestObservability>);
}

TEST(AccessRequestTelemetryTest, ComponentsPreserveW3cAndScriptStateWhenCatIsDisabled) {
    const ComponentResult result = run_component_request();

    EXPECT_TRUE(result.constants_prepared);
    EXPECT_TRUE(result.trace_bound);
    EXPECT_EQ(result.initial_tenant, "blue");
    EXPECT_EQ(result.bound_trace_parent, "00-11111111111111111111111111111111-2222222222222222-01");
    EXPECT_TRUE(result.context_updated);
    EXPECT_EQ(result.updated_tenant, "green");
    EXPECT_TRUE(result.context_removed);
    EXPECT_TRUE(result.upstream_injected);
    EXPECT_TRUE(result.upstream_trace_state.starts_with("vendor=opaque,bnrc="));
    EXPECT_TRUE(result.upstream_trace_id.empty());
    EXPECT_EQ(result.trace_parent, "00-11111111111111111111111111111111-2222222222222222-01");
    EXPECT_TRUE(result.response_finalized);
    EXPECT_TRUE(result.response_trace_id.empty());
    EXPECT_TRUE(result.cat_disabled);
    EXPECT_TRUE(result.provider_disabled);
    EXPECT_EQ(result.copied_value, "request-owned");
}

TEST(AccessRequestTelemetryTest, CatCreationFailureDoesNotAffectW3cOrScriptState) {
    const ComponentResult result = run_component_request(true);

    EXPECT_TRUE(result.cat_disabled);
    EXPECT_TRUE(result.provider_disabled);
    EXPECT_TRUE(result.trace_bound);
    EXPECT_EQ(result.initial_tenant, "blue");
    EXPECT_TRUE(result.upstream_injected);
    EXPECT_TRUE(result.upstream_trace_state.starts_with("vendor=opaque,bnrc="));
    EXPECT_TRUE(result.upstream_trace_id.empty());
}

} // namespace
