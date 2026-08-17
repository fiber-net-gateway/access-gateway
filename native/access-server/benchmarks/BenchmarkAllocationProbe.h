#ifndef FIBER_ACCESS_SERVER_BENCHMARK_ALLOCATION_PROBE_H
#define FIBER_ACCESS_SERVER_BENCHMARK_ALLOCATION_PROBE_H

#include <cstdint>

namespace fiber::access_server::benchmark {

struct AllocationMeasurement {
    std::uint64_t allocations = 0;
    std::uint64_t bytes = 0;
};

void begin_allocation_measurement() noexcept;
[[nodiscard]] AllocationMeasurement finish_allocation_measurement() noexcept;

} // namespace fiber::access_server::benchmark

#endif // FIBER_ACCESS_SERVER_BENCHMARK_ALLOCATION_PROBE_H
