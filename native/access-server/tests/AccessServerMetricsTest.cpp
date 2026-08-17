#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include "observability/AccessServerMetrics.h"

namespace {

using namespace std::chrono_literals;

fiber::async::DetachedTask record_and_collect(fiber::access_server::AccessServerMetrics *metrics,
                                              std::promise<fiber::common::IoResult<std::string>> *done) noexcept {
    using namespace fiber::access_server;

    AccessServerMetrics::Worker &worker = metrics->worker(fiber::event::EventLoop::current().group_index());
    worker.proxy_execution_finished(AccessProxyExecutionResult::Failed);
    worker.proxy_execution_finished(AccessProxyExecutionResult::Canceled);

    worker.proxy_attempt_started();
    worker.proxy_attempt_finished(AccessProxyAttemptResult::Failed);
    worker.proxy_attempt_started();
    worker.proxy_attempt_finished(AccessProxyAttemptResult::Aborted);

    worker.proxy_failure(AccessProxyFailurePhase::BuildHeaders);
    worker.proxy_failure(AccessProxyFailurePhase::WriteResponseBody);
    worker.proxy_pool_acquired(AccessProxyPoolResult::Hit, 2);
    worker.proxy_pool_acquired(AccessProxyPoolResult::Shutdown);
    worker.proxy_dns_resolved(AccessProxyDnsResult::Failure, 3);
    worker.proxy_connect_attempted(AccessProxyConnectResult::TlsFailure);
    worker.proxy_connect_attempted(AccessProxyConnectResult::CreateFailure, 2);
    worker.proxy_connect_candidates_observed(5);
    worker.happy_eyeballs_finished(AccessHappyEyeballsResult::Success);
    worker.happy_eyeballs_finished(AccessHappyEyeballsResult::Failure, 2);

    worker.websocket_handshake_finished(AccessWebSocketHandshakeResult::Rejected);
    worker.websocket_handshake_finished(AccessWebSocketHandshakeResult::Failed);
    worker.websocket_session_started();
    worker.websocket_session_finished(AccessWebSocketSessionResult::Aborted);

    auto collected = co_await metrics->collect(fiber::event::EventLoop::current().io_buf_node_pool());
    fiber::common::IoErr error = fiber::common::IoErr::None;
    std::string output;
    if (!collected) {
        error = collected.error();
    } else {
        while (fiber::mem::IoBuf *part = collected->first_readable()) {
            output.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
            collected->consume_and_compact(part->readable());
        }
    }

    metrics->stop_collecting();
    co_await metrics->wait_for_idle();
    if (error != fiber::common::IoErr::None) {
        done->set_value(std::unexpected(error));
    } else {
        done->set_value(std::move(output));
    }
}

TEST(AccessServerMetricsTest, AggregatesOnlyFixedProxyAndWebSocketDimensions) {
    fiber::event::EventLoopGroup workers(1);
    fiber::access_server::AccessServerMetrics metrics(workers);
    ASSERT_TRUE(metrics.valid());

    std::promise<fiber::common::IoResult<std::string>> promise;
    auto future = promise.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() { return record_and_collect(&metrics, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto collected = future.get();
    ASSERT_TRUE(collected);
    EXPECT_NE(collected->find("access_server_proxy_executions_total{result=\"failed\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_executions_total{result=\"canceled\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_attempts_total{result=\"failed\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_attempts_total{result=\"aborted\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_attempts_inflight 0"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_failures_total{phase=\"build_headers\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_failures_total{phase=\"write_response_body\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_pool_acquires_total{result=\"hit\"} 2"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_pool_acquires_total{result=\"shutdown\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_dns_resolutions_total{result=\"failure\"} 3"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_connect_attempts_total{result=\"tls_failure\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_connect_attempts_total{result=\"create_failure\"} 2"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_connect_candidates_total 5"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_happy_eyeballs_total{result=\"success\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_happy_eyeballs_total{result=\"failure\"} 2"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_handshakes_total{result=\"rejected\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_handshakes_total{result=\"failed\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_sessions_total{result=\"aborted\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_sessions_inflight 0"), std::string::npos);
    EXPECT_EQ(collected->find("project="), std::string::npos);
    EXPECT_EQ(collected->find("host="), std::string::npos);

    workers.stop();
    workers.join();
}

} // namespace
