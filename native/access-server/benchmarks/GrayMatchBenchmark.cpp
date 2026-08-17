#include "BenchmarkSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/common/util/CpuConcurrency.h>
#include <fiber/event/EventLoopGroup.h>

#include "runtime/GrayMatchStore.h"

namespace {

using fiber::access_server::ClientMetadata;
using fiber::access_server::GrayMatchConfig;
using fiber::access_server::GrayMatchConfigEntry;
using fiber::access_server::GrayMatchStore;
using fiber::access_server::ProxyClusterMatcher;
using fiber::access_server::benchmark::Distribution;

constexpr std::uint64_t kDefaultOperationsPerSample = 100'000;
constexpr std::size_t kCidrCount = 256;

enum class CaseKind : std::uint8_t {
    RatioHit,
    FirstCidrHit,
    LastCidrHit,
    EntryMiss,
};

std::string_view case_name(CaseKind kind) noexcept {
    switch (kind) {
        case CaseKind::RatioHit:
            return "ratio_hit";
        case CaseKind::FirstCidrHit:
            return "first_cidr_hit";
        case CaseKind::LastCidrHit:
            return "last_cidr_hit";
        case CaseKind::EntryMiss:
            return "entry_miss";
    }
    return "unknown";
}

GrayMatchConfig make_config() {
    fiber::access_server::NullableStringSet cidrs;
    cidrs.reserve(kCidrCount);
    for (std::size_t index = 0; index < kCidrCount; ++index) {
        cidrs.emplace_back("10." + std::to_string(index) + ".0.0/16");
    }
    return {
            GrayMatchConfigEntry{.entry = "vdi", .ratio = 5000},
            GrayMatchConfigEntry{.entry = "desktop", .ratio = 0, .cidrs = cidrs},
            GrayMatchConfigEntry{.entry = "internet", .ratio = 2500},
            GrayMatchConfigEntry{.entry = "custom", .ratio = 10000},
    };
}

ClientMetadata metadata_for(CaseKind kind) {
    ClientMetadata metadata;
    if (kind == CaseKind::FirstCidrHit) {
        const auto address = fiber::net::IpAddress::v4({10, 0, 1, 1});
        metadata.client_address = address;
        metadata.gray_target = fiber::access_server::Cidr::from_address(address);
    } else if (kind == CaseKind::LastCidrHit) {
        const auto address = fiber::net::IpAddress::v4({10, 255, 1, 1});
        metadata.client_address = address;
        metadata.gray_target = fiber::access_server::Cidr::from_address(address);
    }
    return metadata;
}

std::string_view entry_for(CaseKind kind) noexcept {
    switch (kind) {
        case CaseKind::RatioHit:
            return "vdi";
        case CaseKind::FirstCidrHit:
        case CaseKind::LastCidrHit:
            return "desktop";
        case CaseKind::EntryMiss:
            return "missing";
    }
    return {};
}

struct SampleState {
    SampleState(ProxyClusterMatcher value_matcher, CaseKind value_kind, std::size_t value_workers,
                std::uint64_t value_operations) :
        matcher(value_matcher), kind(value_kind), metadata(metadata_for(value_kind)), workers(value_workers),
        operations(value_operations), checksums(value_workers) {}

    ProxyClusterMatcher matcher;
    CaseKind kind;
    ClientMetadata metadata;
    std::size_t workers;
    std::uint64_t operations;
    std::vector<std::uint64_t> checksums;
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> start{false};
};

struct SampleResult {
    std::uint64_t elapsed_ns = 0;
    std::uint64_t checksum = 0;
};

SampleResult run_sample(fiber::event::EventLoopGroup &group, ProxyClusterMatcher matcher, CaseKind kind,
                        std::size_t worker_count, std::uint64_t operations) {
    auto state = std::make_shared<SampleState>(matcher, kind, worker_count, operations);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        fiber::async::spawn(group.at(worker), [state, worker]() -> fiber::async::DetachedTask {
            state->ready.fetch_add(1, std::memory_order_release);
            state->ready.notify_all();
            while (!state->start.load(std::memory_order_acquire)) {
                co_await fiber::async::yield();
            }

            const std::uint64_t base = state->operations / state->workers;
            const std::uint64_t worker_operations = base + (worker < state->operations % state->workers);
            std::uint64_t checksum = 0;
            const std::string_view entry = entry_for(state->kind);
            for (std::uint64_t operation = 0; operation < worker_operations; ++operation) {
                checksum += state->matcher.matches(state->matcher.context, entry, state->metadata) ? 1U : 0U;
            }
            state->checksums[worker] = checksum;
            state->done.fetch_add(1, std::memory_order_release);
            state->done.notify_all();
            co_return;
        });
    }

    std::size_t ready = state->ready.load(std::memory_order_acquire);
    while (ready != worker_count) {
        state->ready.wait(ready, std::memory_order_acquire);
        ready = state->ready.load(std::memory_order_acquire);
    }
    const auto started = std::chrono::steady_clock::now();
    state->start.store(true, std::memory_order_release);

    std::size_t done = state->done.load(std::memory_order_acquire);
    while (done != worker_count) {
        state->done.wait(done, std::memory_order_acquire);
        done = state->done.load(std::memory_order_acquire);
    }
    SampleResult result{
            .elapsed_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                            .count()),
    };
    for (const std::uint64_t checksum: state->checksums) {
        result.checksum += checksum;
    }
    return result;
}

struct CaseResult {
    Distribution distribution;
    std::uint64_t checksum = 0;
};

CaseResult measure(fiber::event::EventLoopGroup &group, ProxyClusterMatcher matcher, CaseKind kind,
                   std::size_t worker_count, std::uint64_t operations) {
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        const SampleResult result = run_sample(group, matcher, kind, worker_count, operations);
        elapsed.push_back(result.elapsed_ns);
        checksum ^= result.checksum + sample;
    }
    return {
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), operations),
            .checksum = checksum,
    };
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t operations = kDefaultOperationsPerSample;
    const fiber::util::CpuConcurrency cpu = fiber::util::detect_cpu_concurrency();
    std::uint64_t requested_workers = cpu.effective_count;
    if ((argc >= 2 && !fiber::access_server::benchmark::parse_positive(argv[1], operations)) ||
        (argc >= 3 && !fiber::access_server::benchmark::parse_positive(argv[2], requested_workers)) || argc > 3 ||
        requested_workers > std::numeric_limits<std::size_t>::max()) {
        std::fprintf(stderr, "usage: %s [operations-per-sample] [max-workers]\n", argv[0]);
        return 2;
    }
    const std::size_t max_workers =
            std::min<std::size_t>(static_cast<std::size_t>(requested_workers), cpu.effective_count);
    if (operations < max_workers) {
        std::fprintf(stderr, "operations-per-sample must be at least max-workers\n");
        return 2;
    }

    fiber::event::EventLoopGroup group(max_workers);
    GrayMatchStore store(group, {.random_seed = 0x123456789abcdef0ULL});
    const auto applied = store.apply(make_config());
    if (!applied || store.rule_count() != 4) {
        std::fprintf(stderr, "failed to compile gray benchmark fixture\n");
        return 1;
    }
    const ProxyClusterMatcher matcher = store.adapter();
    group.start();

    const std::string_view cpu_source = fiber::util::cpu_concurrency_source_name(cpu.source);
    std::fprintf(stderr, "rules=%zu cidrs_per_cidr_rule=%zu samples=%zu effective_cpus=%zu source=%.*s\n",
                 store.rule_count(), kCidrCount, fiber::access_server::benchmark::kDefaultSamples, cpu.effective_count,
                 static_cast<int>(cpu_source.size()), cpu_source.data());
    std::printf("case,workers,operations,p50_ns_per_operation,p95_ns_per_operation,p99_ns_per_operation,"
                "operations_per_second,checksum\n");
    for (std::size_t workers = 1; workers <= max_workers; ++workers) {
        for (const CaseKind kind:
             {CaseKind::RatioHit, CaseKind::FirstCidrHit, CaseKind::LastCidrHit, CaseKind::EntryMiss}) {
            const CaseResult result = measure(group, matcher, kind, workers, operations);
            const std::string_view name = case_name(kind);
            std::printf("%.*s,%zu,%llu,%.2f,%.2f,%.2f,%.0f,%llu\n", static_cast<int>(name.size()), name.data(), workers,
                        static_cast<unsigned long long>(operations), result.distribution.p50_ns_per_operation,
                        result.distribution.p95_ns_per_operation, result.distribution.p99_ns_per_operation,
                        result.distribution.operations_per_second, static_cast<unsigned long long>(result.checksum));
        }
    }

    group.stop();
    group.join();
    return 0;
}
