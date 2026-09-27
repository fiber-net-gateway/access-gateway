#include "BenchmarkAllocationProbe.h"
#include "BenchmarkSupport.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClient.h>
#include <fiber/cat/CatClientConfig.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpCommon.h>
#include <fiber/log/LogConfig.h>
#include <fiber/log/LogLine.h>
#include <fiber/log/Logger.h>
#include <fiber/log/LoggerManager.h>
#include <fiber/net/IpAddress.h>

#include "observability/AccessLogLine.h"

namespace {

using fiber::access_server::AccessLogRecord;
using fiber::access_server::benchmark::AllocationMeasurement;
using fiber::access_server::benchmark::Distribution;

constexpr std::uint64_t kDefaultLogOperations = 100'000;
constexpr std::uint64_t kDefaultCatOperations = 1'000;

fiber::log::LoggerHandle benchmark_logger{"access.benchmark"};

struct CaseResult {
    Distribution distribution;
    AllocationMeasurement allocation;
    std::uint64_t checksum = 0;
};

enum class LogCase : std::uint8_t {
    Typical,
    LongUri,
};

std::string_view log_case_name(LogCase kind) noexcept {
    switch (kind) {
        case LogCase::Typical:
            return "log_encode_typical";
        case LogCase::LongUri:
            return "log_encode_long_uri";
    }
    return "unknown";
}

std::uint64_t run_log_operations(const AccessLogRecord &record, std::uint64_t operations) {
    // Encodes the Java-compatible line into a reused buffer the way
    // CompatAccessLogAttempt does on the proxy path.
    std::string line;
    line.reserve(384);
    std::uint64_t checksum = 0;
    for (std::uint64_t operation = 0; operation < operations; ++operation) {
        line.clear();
        fiber::access_server::encode_access_log_line(record, 480, line);
        checksum += line.size();
    }
    return checksum;
}

CaseResult measure_log(LogCase kind, std::uint64_t operations) {
    const std::string long_uri = "/api/orders/benchmark?filter=" + std::string(2048, 'a');
    AccessLogRecord record{};
    record.timestamp_ms = 1760000000123;
    record.gateway_name = "access-server-vdi";
    record.trace_id = "3f2a1c9d0b7e4f5a";
    record.hi_trace_id = "0";
    record.trace_parent = "00-3f2a1c9d0b7e4f5a2c9d1e4f7a8b-1b2c3d4e5f607a8b-01";
    record.http_method = "POST";
    record.host = "api.haierxj.cn";
    record.client_ip = "10.24.4.51";
    record.received_path = "/api/orders/benchmark";
    record.server_ip = "172.28.2.111";
    record.app_name = "ploto-orders";
    record.gw_cluster = "default";
    record.ctx_cluster = "-";
    record.ultimate_cluster = "default";
    record.backend_ip = "10.166.204.157";
    record.forward_uri =
            kind == LogCase::Typical ? std::string_view("/api/orders/benchmark") : std::string_view(long_uri);
    record.http_code = 200;
    record.connect_nano = -7;
    record.response_millis = 42;
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        checksum ^= run_log_operations(record, operations) + sample;
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
    }
    fiber::access_server::benchmark::begin_allocation_measurement();
    checksum ^= run_log_operations(record, operations);
    const AllocationMeasurement allocation = fiber::access_server::benchmark::finish_allocation_measurement();
    return {
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), operations),
            .allocation = allocation,
            .checksum = checksum,
    };
}

enum class LoggerCase : std::uint8_t {
    Disabled,
    TenPercent,
    All,
};

std::string_view logger_case_name(LoggerCase kind) noexcept {
    switch (kind) {
        case LoggerCase::Disabled:
            return "logger_disabled";
        case LoggerCase::TenPercent:
            return "logger_sample_10_percent";
        case LoggerCase::All:
            return "logger_all";
    }
    return "unknown";
}

std::uint64_t run_logger_operations(LoggerCase kind, std::uint64_t operations) {
    const fiber::log::Logger &logger = benchmark_logger.get();
    const fiber::log::LogLevel level =
            kind == LoggerCase::Disabled ? fiber::log::LogLevel::Debug : fiber::log::LogLevel::Info;
    std::uint64_t checksum = 0;
    for (std::uint64_t operation = 0; operation < operations; ++operation) {
        const std::uint32_t sample = static_cast<std::uint32_t>((operation * 7919U) % 10000U);
        const bool sampled = kind != LoggerCase::TenPercent || sample < 1000U;
        if (!sampled || !logger.enabled(level)) {
            checksum += 1;
            continue;
        }
        checksum += fiber::log::log_complete_message(logger, level, __FILE__, __LINE__, __func__,
                                                     "request completed status=200 result=success")
                            ? 7U
                            : 3U;
    }
    return checksum;
}

CaseResult measure_logger(LoggerCase kind, std::uint64_t operations) {
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        checksum ^= run_logger_operations(kind, operations) + sample;
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
        fiber::log::LoggerManager::global().flush();
    }
    fiber::access_server::benchmark::begin_allocation_measurement();
    checksum ^= run_logger_operations(kind, operations);
    const AllocationMeasurement allocation = fiber::access_server::benchmark::finish_allocation_measurement();
    fiber::log::LoggerManager::global().flush();
    return {
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), operations),
            .allocation = allocation,
            .checksum = checksum,
    };
}

bool initialize_benchmark_logger() {
    fiber::log::LogConfigBuilder builder;
    auto appender = builder.add_file_appender({
            .name = "benchmark_null",
            .path = "/dev/null",
            .buffer_size = 64 * 1024,
            .flush_interval = std::chrono::hours(1),
            .layout = fiber::log::FileAppenderLayout::MessageOnly,
    });
    if (!appender || !builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*appender}) ||
        !builder.set_async_options({
                .backlog_capacity = 128U << 20U,
                .full_policy = fiber::log::LogQueueFullPolicy::DropNewest,
        })) {
        return false;
    }
    auto config = builder.finish();
    return config && fiber::log::LoggerManager::global().initialize(std::move(*config)).has_value();
}

std::expected<std::unique_ptr<fiber::cat::CatClient>, fiber::cat::CatClientCreateError>
make_cat_client(fiber::event::EventLoop &loop, double sample_rate) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "access-benchmark",
            .hostname = "loopback",
            .ip = "127.0.0.1",
            .thread_group_name = "benchmark",
            .thread_id = "0",
            .thread_name = "worker",
            .bootstrap_collectors =
                    {
                            fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 1),
                    },
    };
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        return std::unexpected(fiber::cat::CatClientCreateError::InvalidOptions);
    }
    fiber::cat::CatClientOptions options;
    options.initial_sample_rate = sample_rate;
    options.enable_heartbeat = false;
    options.enable_system_stats = false;
    options.max_queued_messages = 100'000;
    options.max_queued_bytes = 64U << 20U;
    options.reconnect_initial_delay = std::chrono::hours(1);
    options.reconnect_max_delay = std::chrono::hours(1);
    options.shutdown_drain_timeout = std::chrono::milliseconds::zero();
    return fiber::cat::CatClient::create(loop, std::move(*config), options);
}

std::uint64_t run_cat_operations(fiber::cat::CatClient *client, std::uint64_t operations) {
    if (client == nullptr) {
        std::uint64_t checksum = 0;
        for (std::uint64_t operation = 0; operation < operations; ++operation) {
            checksum += client == nullptr ? 1U : 0U;
        }
        return checksum;
    }

    fiber::mem::BufPool pool;
    std::uint64_t checksum = 0;
    for (std::uint64_t operation = 0; operation < operations; ++operation) {
        pool.reset();
        auto root = client->create_isolated_transaction(pool, "URL", "/benchmark");
        if (!root) {
            checksum += static_cast<std::uint64_t>(root.error()) + 1U;
            continue;
        }
        (void) root->add_data("method", "GET");
        (void) root->log_event("Route", "matched");
        checksum += root->complete() == fiber::cat::RecordError::None ? 7U : 3U;
    }
    return checksum;
}

CaseResult measure_cat_case(fiber::cat::CatClient *client, std::uint64_t operations) {
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        checksum ^= run_cat_operations(client, operations) + sample;
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
    }
    fiber::access_server::benchmark::begin_allocation_measurement();
    checksum ^= run_cat_operations(client, operations);
    const AllocationMeasurement allocation = fiber::access_server::benchmark::finish_allocation_measurement();
    return {
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), operations),
            .allocation = allocation,
            .checksum = checksum,
    };
}

struct CatOutcome {
    std::array<CaseResult, 3> cases;
    fiber::cat::CatClientStats sampled_out_stats;
    fiber::cat::CatClientStats sampled_in_stats;
    bool success = false;
};

fiber::async::DetachedTask run_cat_benchmark(fiber::cat::CatClient *sampled_out, fiber::cat::CatClient *sampled_in,
                                             std::uint64_t operations, CatOutcome *outcome, std::promise<void> *done) {
    const auto out_started = sampled_out->start();
    const auto in_started = sampled_in->start();
    if (!out_started || !in_started) {
        done->set_value();
        co_return;
    }
    outcome->cases[0] = measure_cat_case(nullptr, operations);
    outcome->cases[1] = measure_cat_case(sampled_out, operations);
    outcome->cases[2] = measure_cat_case(sampled_in, operations);
    outcome->sampled_out_stats = sampled_out->stats();
    outcome->sampled_in_stats = sampled_in->stats();
    outcome->success = true;
    co_await sampled_out->shutdown();
    co_await sampled_in->shutdown();
    done->set_value();
}

void print_case(std::string_view name, std::uint64_t operations, const CaseResult &result) {
    std::printf("%.*s,%llu,%.2f,%.2f,%.2f,%.0f,%.4f,%.2f,%llu\n", static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long long>(operations), result.distribution.p50_ns_per_operation,
                result.distribution.p95_ns_per_operation, result.distribution.p99_ns_per_operation,
                result.distribution.operations_per_second,
                static_cast<double>(result.allocation.allocations) / static_cast<double>(operations),
                static_cast<double>(result.allocation.bytes) / static_cast<double>(operations),
                static_cast<unsigned long long>(result.checksum));
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t log_operations = kDefaultLogOperations;
    std::uint64_t cat_operations = kDefaultCatOperations;
    if ((argc >= 2 && !fiber::access_server::benchmark::parse_positive(argv[1], log_operations)) ||
        (argc >= 3 && !fiber::access_server::benchmark::parse_positive(argv[2], cat_operations)) || argc > 3) {
        std::fprintf(stderr, "usage: %s [log-operations-per-sample] [cat-operations-per-sample]\n", argv[0]);
        return 2;
    }

    std::array<CaseResult, 2> log_results;
    constexpr std::array<LogCase, 2> kLogCases{
            LogCase::Typical,
            LogCase::LongUri,
    };
    for (std::size_t index = 0; index < kLogCases.size(); ++index) {
        log_results[index] = measure_log(kLogCases[index], log_operations);
    }

    if (!initialize_benchmark_logger()) {
        std::fprintf(stderr, "failed to initialize benchmark logger\n");
        return 1;
    }
    constexpr std::array<LoggerCase, 3> kLoggerCases{
            LoggerCase::Disabled,
            LoggerCase::TenPercent,
            LoggerCase::All,
    };
    std::array<CaseResult, 3> logger_results;
    for (std::size_t index = 0; index < kLoggerCases.size(); ++index) {
        logger_results[index] = measure_logger(kLoggerCases[index], cat_operations);
    }
    const fiber::log::LogQueueStats logger_stats = fiber::log::LoggerManager::global().queue_stats();
    fiber::log::LoggerManager::global().shutdown();

    fiber::event::EventLoopGroup group(1);
    auto sampled_out = make_cat_client(group.at(0), 0.0);
    auto sampled_in = make_cat_client(group.at(0), 1.0);
    if (!sampled_out || !sampled_in) {
        std::fprintf(stderr, "failed to create CAT benchmark clients\n");
        return 1;
    }
    CatOutcome cat_outcome;
    std::promise<void> done_promise;
    auto done = done_promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_cat_benchmark(sampled_out->get(), sampled_in->get(), cat_operations, &cat_outcome, &done_promise);
    });
    if (done.wait_for(std::chrono::minutes(2)) != std::future_status::ready) {
        std::fprintf(stderr, "CAT benchmark timed out\n");
        group.stop();
        group.join();
        return 1;
    }
    group.stop();
    group.join();
    if (!cat_outcome.success) {
        std::fprintf(stderr, "CAT benchmark failed\n");
        return 1;
    }

    std::printf("case,operations,p50_ns_per_operation,p95_ns_per_operation,p99_ns_per_operation,"
                "operations_per_second,allocations_per_operation,allocated_bytes_per_operation,checksum\n");
    for (std::size_t index = 0; index < kLogCases.size(); ++index) {
        print_case(log_case_name(kLogCases[index]), log_operations, log_results[index]);
    }
    for (std::size_t index = 0; index < kLoggerCases.size(); ++index) {
        print_case(logger_case_name(kLoggerCases[index]), cat_operations, logger_results[index]);
    }
    print_case("cat_disabled", cat_operations, cat_outcome.cases[0]);
    print_case("cat_sampled_out", cat_operations, cat_outcome.cases[1]);
    print_case("cat_sampled_in", cat_operations, cat_outcome.cases[2]);
    std::fprintf(stderr,
                 "samples=%zu logger_peak_queued=%llu logger_dropped=%llu sampled_out_aggregated=%llu "
                 "sampled_in_submitted=%llu sampled_in_queue_full=%llu\n",
                 fiber::access_server::benchmark::kDefaultSamples,
                 static_cast<unsigned long long>(logger_stats.peak_queued_records),
                 static_cast<unsigned long long>(logger_stats.dropped_records),
                 static_cast<unsigned long long>(cat_outcome.sampled_out_stats.aggregated_trees),
                 static_cast<unsigned long long>(cat_outcome.sampled_in_stats.submitted_messages),
                 static_cast<unsigned long long>(cat_outcome.sampled_in_stats.dropped_queue_full));
    return 0;
}
