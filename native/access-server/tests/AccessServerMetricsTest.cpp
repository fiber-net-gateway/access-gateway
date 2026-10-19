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

    auto collected = co_await metrics->collect();
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

fiber::async::DetachedTask
record_request_statuses_and_collect(fiber::access_server::AccessServerMetrics *metrics,
                                    std::promise<fiber::common::IoResult<std::string>> *done) noexcept {
    using namespace fiber::access_server;

    AccessServerMetrics::Worker &worker = metrics->worker(fiber::event::EventLoop::current().group_index());
    const auto finish = [&worker](fiber::http::HttpResponseStats response) {
        worker.request_started();
        worker.request_finished(response, 1ms);
    };
    finish({.status_code = 200, .completed = true});
    finish({.status_code = 200, .completed = true});
    finish({.status_code = 203, .completed = true});
    finish({.status_code = 404, .completed = true});
    finish({.status_code = 418, .completed = true});
    finish({.status_code = 502, .completed = true});
    finish({.status_code = 600, .completed = true});
    finish({.terminal_error = fiber::common::IoErr::ConnReset, .completed = false});

    auto collected = co_await metrics->collect();
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
    EXPECT_NE(collected->find("access_server_proxy_happy_eyeballs_total{result=\"success\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_happy_eyeballs_total{result=\"failure\"} 2"), std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_handshakes_total{result=\"rejected\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_handshakes_total{result=\"failed\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_sessions_total{result=\"aborted\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_websocket_sessions_inflight 0"), std::string::npos);
    EXPECT_EQ(collected->find("project="), std::string::npos);
    EXPECT_EQ(collected->find("host="), std::string::npos);

    workers.stop();
    workers.join();
}

TEST(AccessServerMetricsTest, AggregatesCompletedRequestsByStatusCode) {
    fiber::event::EventLoopGroup workers(1);
    fiber::access_server::AccessServerMetrics metrics(workers);
    ASSERT_TRUE(metrics.valid());

    std::promise<fiber::common::IoResult<std::string>> promise;
    auto future = promise.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() { return record_request_statuses_and_collect(&metrics, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto collected = future.get();
    ASSERT_TRUE(collected);
    EXPECT_NE(collected->find("access_server_requests_total{result=\"success\"} 3"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_total{result=\"client_error\"} 2"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_total{result=\"server_error\"} 2"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_total{result=\"canceled\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_by_status_total{status_code=\"200\"} 2"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_by_status_total{status_code=\"2xx\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_by_status_total{status_code=\"404\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_by_status_total{status_code=\"4xx\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_by_status_total{status_code=\"502\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_by_status_total{status_code=\"other\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_requests_by_status_total{status_code=\"429\"} 0"), std::string::npos);
    EXPECT_EQ(collected->find("status_code=\"600\""), std::string::npos);
    EXPECT_EQ(collected->find("status_code=\"0\""), std::string::npos);

    workers.stop();
    workers.join();
}

fiber::async::DetachedTask
record_dynamic_series_and_collect(fiber::access_server::AccessServerMetrics *metrics,
                                  std::promise<fiber::common::IoResult<std::string>> *done) noexcept {
    using namespace fiber::access_server;

    AccessServerMetrics::Worker &worker = metrics->worker(fiber::event::EventLoop::current().group_index());
    worker.route_request_finished("api.example.com", "/proxy", "200");
    worker.route_request_finished("api.example.com", "/proxy", "200");
    worker.route_request_finished("*.wild.example.com", "/orders/:id", "502");
    // The status is the exact delivered code; the caller derives it the same
    // way RequestObservability::finish does.
    worker.route_request_finished("api.example.com", "/proxy", "101");
    worker.route_request_finished({}, {}, "0");
    worker.route_request_finished({}, "/fallback", "404");

    worker.provider_response_received("inventory", "10.0.0.1:8080", "200");
    worker.provider_response_received("loan.project", "[::1]:9001", "503");

    worker.proxy_transport_error("inventory", "10.0.0.1:8080", "connect");
    worker.proxy_transport_error("inventory", "10.0.0.1:8080", "request_timeout");
    worker.proxy_transport_error("inventory", "10.0.0.1:8080", "io_err");
    // Downstream-origin failures classify to no error label and are dropped.
    worker.proxy_transport_error("inventory", "10.0.0.1:8080", {});

    worker.upstream_circuit_rejected("inventory");
    worker.upstream_circuit_rejected({});

    auto collected = co_await metrics->collect();
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

TEST(AccessServerMetricsTest, ExposesRequestLabeledDynamicSeries) {
    fiber::event::EventLoopGroup workers(1);
    fiber::access_server::AccessServerMetrics metrics(workers);
    ASSERT_TRUE(metrics.valid());

    std::promise<fiber::common::IoResult<std::string>> promise;
    auto future = promise.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() { return record_dynamic_series_and_collect(&metrics, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto collected = future.get();
    ASSERT_TRUE(collected);
    EXPECT_NE(collected->find("access_server_route_requests_total{host=\"api.example.com\",path=\"/proxy\","
                              "status_code=\"200\"} 2"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_route_requests_total{host=\"*.wild.example.com\",path=\"/orders/:id\","
                              "status_code=\"502\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_route_requests_total{host=\"api.example.com\",path=\"/proxy\","
                              "status_code=\"101\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_route_requests_total{host=\"unmatched\",path=\"unmatched\","
                              "status_code=\"0\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_route_requests_total{host=\"unmatched\",path=\"/fallback\","
                              "status_code=\"404\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_provider_responses_total{app=\"inventory\",ip_port=\"10.0.0.1:8080\","
                              "status_code=\"200\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_provider_responses_total{app=\"loan.project\",ip_port=\"[::1]:9001\","
                              "status_code=\"503\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_errors_total{app=\"inventory\",ip_port=\"10.0.0.1:8080\","
                              "error=\"connect\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_errors_total{app=\"inventory\",ip_port=\"10.0.0.1:8080\","
                              "error=\"request_timeout\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_proxy_errors_total{app=\"inventory\",ip_port=\"10.0.0.1:8080\","
                              "error=\"io_err\"} 1"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_upstream_circuit_rejections_total{app=\"inventory\"} 1"),
              std::string::npos);
    // Guarded recorders never emit empty label values.
    EXPECT_EQ(collected->find("app=\"\""), std::string::npos);
    EXPECT_EQ(collected->find("error=\"\""), std::string::npos);

    workers.stop();
    workers.join();
}

fiber::async::DetachedTask
record_body_transfer_and_collect(fiber::access_server::AccessServerMetrics *metrics,
                                 std::promise<fiber::common::IoResult<std::string>> *done) noexcept {
    using namespace fiber::access_server;

    AccessServerMetrics::Worker &worker = metrics->worker(fiber::event::EventLoop::current().group_index());
    // Fixed families: request legs 100/90 (10 bytes dropped mid-transfer),
    // response legs 500/120 (compression plus a truncated remainder).
    worker.request_body_transferred(AccessBodySide::Downstream, 60);
    worker.request_body_transferred(AccessBodySide::Downstream, 40);
    worker.request_body_transferred(AccessBodySide::Upstream, 90);
    worker.response_body_transferred(AccessBodySide::Upstream, 500);
    worker.response_body_transferred(AccessBodySide::Downstream, 120);
    worker.response_compression_bytes(AccessCompressionStream::Raw, 300);
    worker.response_compression_bytes(AccessCompressionStream::Compressed, 75);
    // Zero-byte recordings must not move any counter.
    worker.request_body_transferred(AccessBodySide::Downstream, 0);
    worker.response_body_transferred(AccessBodySide::Downstream, 0);
    worker.response_compression_bytes(AccessCompressionStream::Raw, 0);
    // Size histograms: 5 bytes lands in the first bucket, 5 MiB in the 8 MiB one.
    worker.request_body_size(5);
    worker.request_body_size(0);
    worker.response_body_size(5 * 1024 * 1024);

    // Dynamic body families: per-direction values with the same unmatched
    // placeholder rules as the request-count family, plus zero-byte drops.
    worker.route_body_transferred("api.example.com", "/proxy", AccessBodyDirection::Request, 4096);
    worker.route_body_transferred("api.example.com", "/proxy", AccessBodyDirection::Request, 1024);
    worker.route_body_transferred("api.example.com", "/proxy", AccessBodyDirection::Response, 65536);
    worker.route_body_transferred({}, {}, AccessBodyDirection::Response, 2048);
    worker.route_body_transferred("api.example.com", "/proxy", AccessBodyDirection::Request, 0);
    worker.provider_body_transferred("inventory", "10.0.0.1:8080", AccessBodyDirection::Request, 4096);
    worker.provider_body_transferred("inventory", "10.0.0.1:8080", AccessBodyDirection::Response, 262144);
    worker.provider_body_transferred("inventory", "10.0.0.1:8080", AccessBodyDirection::Response, 0);
    worker.provider_body_transferred({}, "10.0.0.1:8080", AccessBodyDirection::Response, 2048);

    auto collected = co_await metrics->collect();
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

TEST(AccessServerMetricsTest, ExposesBodyTransferFamilies) {
    fiber::event::EventLoopGroup workers(1);
    fiber::access_server::AccessServerMetrics metrics(workers);
    ASSERT_TRUE(metrics.valid());

    std::promise<fiber::common::IoResult<std::string>> promise;
    auto future = promise.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() { return record_body_transfer_and_collect(&metrics, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto collected = future.get();
    ASSERT_TRUE(collected);
    EXPECT_NE(collected->find("access_server_request_body_bytes_total{side=\"downstream\"} 100"), std::string::npos);
    EXPECT_NE(collected->find("access_server_request_body_bytes_total{side=\"upstream\"} 90"), std::string::npos);
    EXPECT_NE(collected->find("access_server_response_body_bytes_total{side=\"upstream\"} 500"), std::string::npos);
    EXPECT_NE(collected->find("access_server_response_body_bytes_total{side=\"downstream\"} 120"), std::string::npos);
    EXPECT_NE(collected->find("access_server_response_compression_bytes_total{stream=\"raw\"} 300"), std::string::npos);
    EXPECT_NE(collected->find("access_server_response_compression_bytes_total{stream=\"compressed\"} 75"),
              std::string::npos);
    // Size histograms: exact bucket counts for the observed nonzero sizes.
    EXPECT_NE(collected->find("access_server_request_body_size_bytes_bucket{le=\"1024\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_request_body_size_bytes_count 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_request_body_size_bytes_sum 5"), std::string::npos);
    EXPECT_NE(collected->find("access_server_response_body_size_bytes_bucket{le=\"4194304\"} 0"), std::string::npos);
    EXPECT_NE(collected->find("access_server_response_body_size_bytes_bucket{le=\"16777216\"} 1"), std::string::npos);
    EXPECT_NE(collected->find("access_server_response_body_size_bytes_count 1"), std::string::npos);
    // Dynamic body series: nonzero per-direction sums, unmatched placeholders,
    // and no series for zero-byte or empty-label recordings.
    EXPECT_NE(collected->find("access_server_route_body_bytes_total{host=\"api.example.com\",path=\"/proxy\","
                              "direction=\"request\"} 5120"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_route_body_bytes_total{host=\"api.example.com\",path=\"/proxy\","
                              "direction=\"response\"} 65536"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_route_body_bytes_total{host=\"unmatched\",path=\"unmatched\","
                              "direction=\"response\"} 2048"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_provider_body_bytes_total{app=\"inventory\",ip_port=\"10.0.0.1:8080\","
                              "direction=\"request\"} 4096"),
              std::string::npos);
    EXPECT_NE(collected->find("access_server_provider_body_bytes_total{app=\"inventory\",ip_port=\"10.0.0.1:8080\","
                              "direction=\"response\"} 262144"),
              std::string::npos);
    EXPECT_EQ(collected->find("direction=\"\""), std::string::npos);

    workers.stop();
    workers.join();
}

} // namespace
