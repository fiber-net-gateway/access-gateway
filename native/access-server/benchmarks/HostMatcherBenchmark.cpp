#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "BenchmarkSupport.h"
#include "routing/HostMatcher.h"

namespace {

using fiber::access_server::HostMatcher;
using fiber::access_server::HostPattern;

constexpr std::size_t kQueries = 4096;
constexpr std::size_t kLookupsPerSample = 65536;
constexpr std::array<std::size_t, 8> kFanouts{1, 4, 8, 16, 32, 64, 256, 1024};

std::uint64_t checksum_sink = 0;

struct Query {
    std::string host;
    std::optional<std::uint32_t> expected;
};

struct Fixture {
    HostMatcher matcher;
    std::vector<Query> queries;
};

Fixture make_fixture(std::size_t fanout) {
    std::vector<std::string> storage;
    storage.reserve(fanout);
    for (std::size_t offset = 0; offset < fanout; ++offset) {
        const std::size_t index = fanout - offset - 1;
        storage.push_back("tenant-" + std::to_string(index) + ".fanout.benchmark");
    }

    std::vector<HostPattern> patterns;
    patterns.reserve(storage.size());
    for (std::size_t offset = 0; offset < storage.size(); ++offset) {
        patterns.push_back(HostPattern{
                .pattern = storage[offset],
                .handler = static_cast<std::uint32_t>(fanout - offset),
        });
    }
    auto built = HostMatcher::build(patterns);
    if (!built) {
        std::abort();
    }

    Fixture fixture{.matcher = std::move(*built)};
    fixture.queries.reserve(kQueries);
    std::uint32_t random = 0x9e3779b9U;
    for (std::size_t index = 0; index < kQueries; ++index) {
        random = random * 1664525U + 1013904223U;
        if ((index & 3U) == 0) {
            fixture.queries.push_back(Query{
                    .host = "missing-" + std::to_string(random) + ".fanout.benchmark",
                    .expected = std::nullopt,
            });
            continue;
        }
        const std::uint32_t handler = random % static_cast<std::uint32_t>(fanout) + 1U;
        fixture.queries.push_back(Query{
                .host = "TENANT-" + std::to_string(handler - 1U) + ".FANOUT.BENCHMARK",
                .expected = handler,
        });
    }
    return fixture;
}

fiber::access_server::benchmark::Distribution measure(std::size_t fanout) {
    Fixture fixture = make_fixture(fanout);
    for (const Query &query: fixture.queries) {
        if (fixture.matcher.match(query.host) != query.expected) {
            std::abort();
        }
    }

    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t lookup = 0; lookup < kLookupsPerSample; ++lookup) {
            const std::optional<std::uint32_t> matched = fixture.matcher.match(fixture.queries[lookup % kQueries].host);
            checksum += matched ? static_cast<std::uint64_t>(*matched) + 1U : 1U;
        }
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
    }
    checksum_sink += checksum;
    return fiber::access_server::benchmark::summarize(std::move(elapsed), kLookupsPerSample);
}

} // namespace

int main() {
    std::printf("fanout,lookups_per_sample,p50_ns_per_lookup,p95_ns_per_lookup,p99_ns_per_lookup,"
                "lookups_per_second\n");
    for (const std::size_t fanout: kFanouts) {
        const auto result = measure(fanout);
        std::printf("%zu,%zu,%.2f,%.2f,%.2f,%.0f\n", fanout, kLookupsPerSample, result.p50_ns_per_operation,
                    result.p95_ns_per_operation, result.p99_ns_per_operation, result.operations_per_second);
    }
    std::fprintf(stderr, "checksum=%llu\n", static_cast<unsigned long long>(checksum_sink));
    return 0;
}
