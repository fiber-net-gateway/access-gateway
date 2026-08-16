#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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

namespace {

constexpr std::array<std::size_t, 4> kEndpointCounts{1, 8, 32, 128};
constexpr std::uint64_t kDefaultOperationsPerCase = 100'000;

bool parse_positive(std::string_view input, std::uint64_t &value) noexcept {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), parsed);
    if (error != std::errc{} || end != input.data() + input.size() || parsed == 0) {
        return false;
    }
    value = parsed;
    return true;
}

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
              std::uint64_t value_operations) :
        state(&value_state), workers(value_workers), operations(value_operations), elapsed_ns(value_workers),
        checksums(value_workers) {}

    fiber::access_server::AccessServiceState *state = nullptr;
    std::size_t workers = 0;
    std::uint64_t operations = 0;
    std::vector<std::uint64_t> elapsed_ns;
    std::vector<std::uint64_t> checksums;
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
                    std::size_t worker_count, std::uint64_t operations) {
    auto run = std::make_shared<CaseState>(state, worker_count, operations);
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

double operations_per_second(std::uint64_t operations, std::uint64_t elapsed_ns) noexcept {
    if (elapsed_ns == 0) {
        return 0.0;
    }
    return static_cast<double>(operations) * 1'000'000'000.0 / static_cast<double>(elapsed_ns);
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t operations_per_case = kDefaultOperationsPerCase;
    const fiber::util::CpuConcurrency cpu = fiber::util::detect_cpu_concurrency();
    const std::size_t detected_cpus = cpu.effective_count;
    std::uint64_t requested_workers = detected_cpus;
    if ((argc >= 2 && !parse_positive(argv[1], operations_per_case)) ||
        (argc >= 3 && !parse_positive(argv[2], requested_workers)) || argc > 3 ||
        requested_workers > std::numeric_limits<std::size_t>::max()) {
        std::fprintf(stderr, "usage: %s [operations-per-case] [max-workers]\n", argv[0]);
        return 2;
    }

    const std::size_t max_workers = std::min<std::size_t>(static_cast<std::size_t>(requested_workers), detected_cpus);
    const std::string_view cpu_source = fiber::util::cpu_concurrency_source_name(cpu.source);
    std::fprintf(stderr, "effective_cpus=%zu source=%.*s\n", detected_cpus, static_cast<int>(cpu_source.size()),
                 cpu_source.data());
    fiber::access_server::AccessServiceState state;
    state.initialize({}, "sh");
    fiber::event::EventLoopGroup group(max_workers);
    group.start();

    std::printf("endpoints,workers,operations,elapsed_ns,operations_per_second,checksum\n");
    bool failed = false;
    for (const std::size_t endpoint_count: kEndpointCounts) {
        ServiceSnapshot snapshot(endpoint_count);
        state.update(snapshot.info);
        for (std::size_t workers = 1; workers <= max_workers; ++workers) {
            const CaseResult result = run_case(group, state, workers, operations_per_case);
            std::printf("%zu,%zu,%llu,%llu,%.0f,%llu\n", endpoint_count, workers,
                        static_cast<unsigned long long>(operations_per_case),
                        static_cast<unsigned long long>(result.elapsed_ns),
                        operations_per_second(operations_per_case, result.elapsed_ns),
                        static_cast<unsigned long long>(result.checksum));
            failed = failed || result.failed;
        }
    }

    group.stop();
    group.join();
    state.retire(fiber::nacos::ServiceRetireReason::Shutdown);
    return failed ? 1 : 0;
}
