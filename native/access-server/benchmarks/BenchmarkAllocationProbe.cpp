#include "BenchmarkAllocationProbe.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

namespace {

std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> allocations{0};
std::atomic<std::uint64_t> bytes{0};

void record(std::size_t size) noexcept {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    allocations.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
}

void *allocate(std::size_t size) {
    const std::size_t actual_size = size == 0 ? 1 : size;
    void *result = std::malloc(actual_size);
    if (result == nullptr) {
        std::abort();
    }
    record(actual_size);
    return result;
}

void *allocate_aligned(std::size_t size, std::size_t alignment) {
    const std::size_t actual_size = size == 0 ? 1 : size;
    const std::size_t remainder = actual_size % alignment;
    if (remainder != 0 && actual_size > std::numeric_limits<std::size_t>::max() - (alignment - remainder)) {
        std::abort();
    }
    const std::size_t rounded = remainder == 0 ? actual_size : actual_size + alignment - remainder;
    void *result = std::aligned_alloc(alignment, rounded);
    if (result == nullptr) {
        std::abort();
    }
    record(rounded);
    return result;
}

} // namespace

namespace fiber::access_server::benchmark {

void begin_allocation_measurement() noexcept {
    enabled.store(false, std::memory_order_relaxed);
    allocations.store(0, std::memory_order_relaxed);
    bytes.store(0, std::memory_order_relaxed);
    enabled.store(true, std::memory_order_release);
}

AllocationMeasurement finish_allocation_measurement() noexcept {
    enabled.store(false, std::memory_order_release);
    return {
            .allocations = allocations.load(std::memory_order_relaxed),
            .bytes = bytes.load(std::memory_order_relaxed),
    };
}

} // namespace fiber::access_server::benchmark

void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void *operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }
void operator delete(void *value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void *value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void *value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t, std::align_val_t) noexcept { std::free(value); }
