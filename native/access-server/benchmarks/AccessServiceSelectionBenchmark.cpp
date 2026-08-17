#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/common/util/CpuConcurrency.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/nacos/NamingService.h>

#include "runtime/AccessServiceDiscovery.h"

#include "BenchmarkSupport.h"

namespace {

constexpr std::array<std::size_t, 4> kEndpointCounts{1, 8, 32, 128};
constexpr std::uint64_t kDefaultOperationsPerCase = 100'000;

struct ServiceSnapshot {
    explicit ServiceSnapshot(std::size_t endpoint_count) {
        instances.reserve(endpoint_count);
        for (std::size_t index = 0; index < endpoint_count; ++index) {
            const std::size_t third_octet = index / 250;
            const std::size_t fourth_octet = index % 250 + 1;
            instances.push_back(fiber::nacos::Instance{
                    .instance_id = "endpoint-" + std::to_string(index),
                    .ip = "10.0." + std::to_string(third_octet) + "." + std::to_string(fourth_octet),
                    .port = 8080,
                    .weight = static_cast<double>(index % 8 + 1),
                    .cluster_name = "sh-default",
            });
        }

        hosts.reserve(instances.size());
        for (const fiber::nacos::Instance &instance: instances) {
            hosts.push_back(fiber::nacos::ServiceInstance{
                    .instance_id = instance.instance_id,
                    .ip = instance.ip,
                    .port = instance.port,
                    .weight = instance.weight,
                    .healthy = instance.healthy,
                    .enabled = instance.enabled,
                    .ephemeral = instance.ephemeral,
                    .cluster_name = instance.cluster_name,
                    .service_name = instance.service_name,
            });
        }
        info.name = "selection-benchmark";
        info.group_name = "DEFAULT_GROUP";
        info.hosts = hosts;
        checksum = "endpoints-" + std::to_string(endpoint_count);
        info.checksum = checksum;
    }

    std::vector<fiber::nacos::Instance> instances;
    std::vector<fiber::nacos::ServiceInstance> hosts;
    std::string checksum;
    fiber::nacos::ServiceInfo info;
};

struct CaseState {
    CaseState(fiber::access_server::AccessServiceState &value_state, std::size_t value_workers,
              std::uint64_t value_operations, bool value_report_success) :
        state(&value_state), workers(value_workers), operations(value_operations), elapsed_ns(value_workers),
        checksums(value_workers), report_success(value_report_success) {}

    fiber::access_server::AccessServiceState *state = nullptr;
    std::size_t workers = 0;
    std::uint64_t operations = 0;
    std::vector<std::uint64_t> elapsed_ns;
    std::vector<std::uint64_t> checksums;
    bool report_success = false;
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
};

struct CaseResult {
    std::uint64_t elapsed_ns = 0;
    std::uint64_t checksum = 0;
    bool failed = false;
};

CaseResult run_case(fiber::event::EventLoopGroup &group, fiber::access_server::AccessServiceState &state,
                    std::size_t worker_count, std::uint64_t operations, bool report_success) {
    auto run = std::make_shared<CaseState>(state, worker_count, operations, report_success);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        fiber::async::spawn(group.at(worker), [run, worker]() -> fiber::async::DetachedTask {
            run->ready.fetch_add(1, std::memory_order_release);
            run->ready.notify_all();
            while (!run->start.load(std::memory_order_acquire)) {
                co_await fiber::async::yield();
            }

            const std::uint64_t base_operations = run->operations / run->workers;
            const std::uint64_t worker_operations = base_operations + (worker < run->operations % run->workers);
            std::uint64_t checksum = 0;
            const auto started = std::chrono::steady_clock::now();
            for (std::uint64_t operation = 0; operation < worker_operations; ++operation) {
                auto selected = run->state->select("default", {});
                if (!selected) {
                    run->failed.store(true, std::memory_order_relaxed);
                    continue;
                }
                checksum += selected->selection_token();
                if (run->report_success) {
                    selected->report(true);
                }
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;
            run->elapsed_ns[worker] =
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            run->checksums[worker] = checksum;
            run->done.fetch_add(1, std::memory_order_acq_rel);
            run->done.notify_all();
            co_return;
        });
    }

    std::size_t ready = run->ready.load(std::memory_order_acquire);
    while (ready != worker_count) {
        run->ready.wait(ready, std::memory_order_acquire);
        ready = run->ready.load(std::memory_order_acquire);
    }
    run->start.store(true, std::memory_order_release);

    std::size_t done = run->done.load(std::memory_order_acquire);
    while (done != worker_count) {
        run->done.wait(done, std::memory_order_acquire);
        done = run->done.load(std::memory_order_acquire);
    }

    CaseResult result;
    result.failed = run->failed.load(std::memory_order_relaxed);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        result.elapsed_ns = std::max(result.elapsed_ns, run->elapsed_ns[worker]);
        result.checksum += run->checksums[worker];
    }
    return result;
}

struct Measurement {
    fiber::access_server::benchmark::Distribution distribution;
    std::uint64_t checksum = 0;
    bool failed = false;
};

Measurement measure(fiber::event::EventLoopGroup &group, fiber::access_server::AccessServiceState &state,
                    std::size_t worker_count, std::uint64_t operations, bool report_success) {
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
    std::uint64_t checksum = 0;
    bool failed = false;
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        const CaseResult result = run_case(group, state, worker_count, operations, report_success);
        elapsed.push_back(result.elapsed_ns);
        checksum ^= result.checksum + sample;
        failed = failed || result.failed;
    }
    return {
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), operations),
            .checksum = checksum,
            .failed = failed,
    };
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t operations_per_case = kDefaultOperationsPerCase;
    const fiber::util::CpuConcurrency cpu = fiber::util::detect_cpu_concurrency();
    const std::size_t detected_cpus = cpu.effective_count;
    std::uint64_t requested_workers = detected_cpus;
    if ((argc >= 2 && !fiber::access_server::benchmark::parse_positive(argv[1], operations_per_case)) ||
        (argc >= 3 && !fiber::access_server::benchmark::parse_positive(argv[2], requested_workers)) || argc > 3 ||
        requested_workers > std::numeric_limits<std::size_t>::max()) {
        std::fprintf(stderr, "usage: %s [operations-per-case] [max-workers]\n", argv[0]);
        return 2;
    }

    const std::size_t max_workers = std::min<std::size_t>(static_cast<std::size_t>(requested_workers), detected_cpus);
    if (operations_per_case < max_workers) {
        std::fprintf(stderr, "operations-per-case must be at least max-workers\n");
        return 2;
    }
    const std::string_view cpu_source = fiber::util::cpu_concurrency_source_name(cpu.source);
    std::fprintf(stderr, "effective_cpus=%zu source=%.*s\n", detected_cpus, static_cast<int>(cpu_source.size()),
                 cpu_source.data());
    fiber::event::EventLoopGroup group(max_workers);
    fiber::access_server::AccessServiceState canonical_state;
    canonical_state.initialize({}, "sh");
    fiber::access_server::AccessServiceState sharded_state;
    sharded_state.initialize({}, "sh", {}, &group);
    group.start();

    std::printf("mode,completion,endpoints,workers,operations,p50_ns_per_operation,p95_ns_per_operation,"
                "p99_ns_per_operation,operations_per_second,throughput_scaling_efficiency,checksum\n");
    bool failed = false;
    for (const std::size_t endpoint_count: kEndpointCounts) {
        ServiceSnapshot snapshot(endpoint_count);
        canonical_state.update(snapshot.info);
        sharded_state.update(snapshot.info);
        for (const bool report_success: {false, true}) {
            const std::array modes{
                    std::pair<std::string_view, fiber::access_server::AccessServiceState *>{"canonical",
                                                                                            &canonical_state},
                    std::pair<std::string_view, fiber::access_server::AccessServiceState *>{"worker_sharded",
                                                                                            &sharded_state},
            };
            for (const auto &[mode, state]: modes) {
                double single_worker_throughput = 0.0;
                for (std::size_t workers = 1; workers <= max_workers; ++workers) {
                    const Measurement result = measure(group, *state, workers, operations_per_case, report_success);
                    if (workers == 1) {
                        single_worker_throughput = result.distribution.operations_per_second;
                    }
                    const double scaling_efficiency =
                            single_worker_throughput == 0.0
                                    ? 0.0
                                    : result.distribution.operations_per_second /
                                              (single_worker_throughput * static_cast<double>(workers));
                    const std::string_view completion = report_success ? "select_report" : "select_only";
                    std::printf("%.*s,%.*s,%zu,%zu,%llu,%.2f,%.2f,%.2f,%.0f,%.4f,%llu\n", static_cast<int>(mode.size()),
                                mode.data(), static_cast<int>(completion.size()), completion.data(), endpoint_count,
                                workers, static_cast<unsigned long long>(operations_per_case),
                                result.distribution.p50_ns_per_operation, result.distribution.p95_ns_per_operation,
                                result.distribution.p99_ns_per_operation, result.distribution.operations_per_second,
                                scaling_efficiency, static_cast<unsigned long long>(result.checksum));
                    failed = failed || result.failed;
                }
            }
        }
    }

    group.stop();
    group.join();
    canonical_state.retire(fiber::nacos::ServiceRetireReason::Shutdown);
    sharded_state.retire(fiber::nacos::ServiceRetireReason::Shutdown);
    return failed ? 1 : 0;
}
