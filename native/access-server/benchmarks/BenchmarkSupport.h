#ifndef FIBER_ACCESS_SERVER_BENCHMARK_SUPPORT_H
#define FIBER_ACCESS_SERVER_BENCHMARK_SUPPORT_H

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fiber::access_server::benchmark {

inline constexpr std::size_t kDefaultSamples = 21;

struct Distribution {
    double p50_ns_per_operation = 0;
    double p95_ns_per_operation = 0;
    double p99_ns_per_operation = 0;
    double operations_per_second = 0;
};

inline bool parse_positive(std::string_view input, std::uint64_t &value) noexcept {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), parsed);
    if (error != std::errc{} || end != input.data() + input.size() || parsed == 0) {
        return false;
    }
    value = parsed;
    return true;
}

inline std::size_t percentile_index(std::size_t count, std::size_t percentile) noexcept {
    return (count * percentile + 99U) / 100U - 1U;
}

inline Distribution summarize(std::vector<std::uint64_t> elapsed_ns, std::uint64_t operations_per_sample) {
    std::sort(elapsed_ns.begin(), elapsed_ns.end());
    const auto per_operation = [operations_per_sample](std::uint64_t elapsed) {
        return static_cast<double>(elapsed) / static_cast<double>(operations_per_sample);
    };
    const std::uint64_t p50 = elapsed_ns[percentile_index(elapsed_ns.size(), 50)];
    return {
            .p50_ns_per_operation = per_operation(p50),
            .p95_ns_per_operation = per_operation(elapsed_ns[percentile_index(elapsed_ns.size(), 95)]),
            .p99_ns_per_operation = per_operation(elapsed_ns[percentile_index(elapsed_ns.size(), 99)]),
            .operations_per_second =
                    p50 == 0 ? 0.0
                             : static_cast<double>(operations_per_sample) * 1'000'000'000.0 / static_cast<double>(p50),
    };
}

} // namespace fiber::access_server::benchmark

#endif // FIBER_ACCESS_SERVER_BENCHMARK_SUPPORT_H
