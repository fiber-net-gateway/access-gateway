#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "execution/ProxyResponsePlan.h"
#include "execution/ResponsePlan.h"
#include "execution/TemplateEvaluator.h"
#include "routing/CompiledHeaderTemplates.h"
#include "routing/CompiledTemplate.h"
#include "routing/ProjectRouteSnapshot.h"

namespace allocation_probe {

thread_local bool enabled = false;
thread_local std::uint64_t allocations = 0;
thread_local std::uint64_t bytes = 0;

void record(std::size_t size) noexcept {
    if (enabled) {
        ++allocations;
        bytes += size;
    }
}

void *allocate(std::size_t size) noexcept {
    const std::size_t actual_size = size == 0 ? 1 : size;
    void *result = std::malloc(actual_size);
    if (result == nullptr) {
        std::abort();
    }
    record(actual_size);
    return result;
}

void *allocate_aligned(std::size_t size, std::size_t alignment) noexcept {
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

struct Measurement {
    std::uint64_t allocation_count = 0;
    std::uint64_t allocated_bytes = 0;
};

class Scope {
public:
    Scope() noexcept {
        allocations = 0;
        bytes = 0;
        enabled = true;
    }

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

    ~Scope() { enabled = false; }

    [[nodiscard]] Measurement finish() noexcept {
        enabled = false;
        return {
                .allocation_count = allocations,
                .allocated_bytes = bytes,
        };
    }
};

} // namespace allocation_probe

void *operator new(std::size_t size) { return allocation_probe::allocate(size); }
void *operator new[](std::size_t size) { return allocation_probe::allocate(size); }
void *operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }
void operator delete(void *value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void *value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void *value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t, std::align_val_t) noexcept { std::free(value); }

namespace {

using fiber::access_server::CompiledHeaderTemplates;
using fiber::access_server::CompiledResponseHeaderTemplate;
using fiber::access_server::CompiledResponseRoute;
using fiber::access_server::CompiledTemplate;
using fiber::access_server::EvaluatedHeader;
using fiber::access_server::Result;
using fiber::access_server::TemplateEvaluator;

constexpr std::uint64_t kDefaultOperations = 10'000;
constexpr std::size_t kSamples = 31;
constexpr std::string_view kExpressionValue = "0123456789abcdef0123456789abcdef0123456789abcdef";

std::uint64_t checksum_sink = 0;

bool parse_positive(std::string_view input, std::uint64_t &value) noexcept {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), parsed);
    if (error != std::errc{} || end != input.data() + input.size() || parsed == 0) {
        return false;
    }
    value = parsed;
    return true;
}

CompiledTemplate compile_template(std::string_view source) {
    auto compiled = fiber::access_server::parse_template(source);
    if (!compiled) {
        std::abort();
    }
    return std::move(*compiled);
}

CompiledResponseHeaderTemplate response_header(std::string name, std::string_view source) {
    return CompiledResponseHeaderTemplate(std::move(name), compile_template(source));
}

Result<void> evaluate_expression(void *, const fiber::script::Script &, std::string_view,
                                 std::string &output) noexcept {
    output.append(kExpressionValue);
    return {};
}

TemplateEvaluator evaluator() noexcept {
    return {
            .evaluate = evaluate_expression,
    };
}

CompiledResponseRoute response_route(bool dynamic) {
    CompiledResponseRoute response{
            .status = 200,
            .body_kind = fiber::access_server::ResponseBodyKind::Text,
            .body = "benchmark-response-body",
    };
    response.response_headers.reserve(8);
    for (std::size_t index = 0; index < 8; ++index) {
        response.response_headers.push_back(response_header("X-Benchmark-Header-" + std::to_string(index),
                                                            dynamic ? "prefix-${value}-suffix" : kExpressionValue));
    }
    return response;
}

CompiledHeaderTemplates proxy_headers(bool dynamic) {
    CompiledHeaderTemplates::Builder builder(8);
    for (std::size_t index = 0; index < 8; ++index) {
        auto inserted = builder.insert("X-Benchmark-Header-" + std::to_string(index),
                                       compile_template(dynamic ? "prefix-${value}-suffix" : kExpressionValue));
        if (!inserted) {
            std::abort();
        }
    }
    return std::move(builder).build();
}

std::uint64_t header_checksum(const std::vector<EvaluatedHeader> &headers) noexcept {
    std::uint64_t checksum = headers.size();
    for (const EvaluatedHeader &header: headers) {
        checksum += header.name.size() + header.value.view().size();
    }
    return checksum;
}

struct CaseResult {
    double p50_ns_per_operation = 0;
    double p95_ns_per_operation = 0;
    double p99_ns_per_operation = 0;
    double operations_per_second = 0;
    double allocations_per_operation = 0;
    double bytes_per_operation = 0;
    std::uint64_t checksum = 0;
};

template<typename Operation>
CaseResult run_case(std::uint64_t operations, Operation operation) {
    for (std::uint64_t index = 0; index < std::min<std::uint64_t>(operations, 1000); ++index) {
        checksum_sink ^= operation();
    }

    allocation_probe::Scope allocation_scope;
    std::uint64_t allocation_checksum = 0;
    for (std::uint64_t index = 0; index < operations; ++index) {
        allocation_checksum += operation();
    }
    const allocation_probe::Measurement allocation = allocation_scope.finish();

    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(kSamples);
    std::uint64_t timing_checksum = 0;
    for (std::size_t sample = 0; sample < kSamples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < operations; ++index) {
            timing_checksum += operation();
        }
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
    }
    std::sort(elapsed.begin(), elapsed.end());
    checksum_sink ^= allocation_checksum ^ timing_checksum;
    const std::uint64_t p50 = elapsed[kSamples / 2];
    const std::uint64_t p95 = elapsed[(kSamples * 95U + 99U) / 100U - 1U];
    const std::uint64_t p99 = elapsed[(kSamples * 99U + 99U) / 100U - 1U];
    return {
            .p50_ns_per_operation = static_cast<double>(p50) / static_cast<double>(operations),
            .p95_ns_per_operation = static_cast<double>(p95) / static_cast<double>(operations),
            .p99_ns_per_operation = static_cast<double>(p99) / static_cast<double>(operations),
            .operations_per_second =
                    p50 == 0 ? 0.0 : static_cast<double>(operations) * 1'000'000'000.0 / static_cast<double>(p50),
            .allocations_per_operation =
                    static_cast<double>(allocation.allocation_count) / static_cast<double>(operations),
            .bytes_per_operation = static_cast<double>(allocation.allocated_bytes) / static_cast<double>(operations),
            .checksum = allocation_checksum ^ timing_checksum,
    };
}

void print_case(std::string_view name, const CaseResult &result) {
    std::printf("%.*s,%.1f,%.1f,%.1f,%.0f,%.3f,%.1f,%llu\n", static_cast<int>(name.size()), name.data(),
                result.p50_ns_per_operation, result.p95_ns_per_operation, result.p99_ns_per_operation,
                result.operations_per_second, result.allocations_per_operation, result.bytes_per_operation,
                static_cast<unsigned long long>(result.checksum));
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t operations = kDefaultOperations;
    if (argc > 2 || (argc == 2 && !parse_positive(argv[1], operations))) {
        std::fprintf(stderr, "usage: %s [operations-per-sample]\n", argv[0]);
        return 2;
    }

    const CompiledTemplate static_template =
            compile_template("static-template-value-0123456789abcdef0123456789abcdef0123456789abcdef");
    const CompiledTemplate dynamic_template = compile_template("a=${first};b=${second};c=${third};d=${fourth}");
    const CompiledResponseRoute static_response = response_route(false);
    const CompiledResponseRoute dynamic_response = response_route(true);
    const CompiledHeaderTemplates static_proxy_headers = proxy_headers(false);
    const CompiledHeaderTemplates dynamic_proxy_headers = proxy_headers(true);

    std::printf("case,p50_ns_per_operation,p95_ns_per_operation,p99_ns_per_operation,operations_per_second,"
                "allocations_per_operation,allocated_bytes_per_operation,checksum\n");
    print_case("static_template", run_case(operations, [&] {
                   auto result = fiber::access_server::evaluate_template(static_template, {});
                   return result ? static_cast<std::uint64_t>(result->size()) : 0;
               }));
    print_case("dynamic_template", run_case(operations, [&] {
                   auto result = fiber::access_server::evaluate_template(dynamic_template, evaluator());
                   return result ? static_cast<std::uint64_t>(result->size()) : 0;
               }));
    print_case("static_response_headers", run_case(operations, [&] {
                   auto result = fiber::access_server::prepare_response(static_response, {});
                   return result ? header_checksum(result->headers) : 0;
               }));
    print_case("dynamic_response_headers", run_case(operations, [&] {
                   auto result = fiber::access_server::prepare_response(dynamic_response, evaluator());
                   return result ? header_checksum(result->headers) : 0;
               }));
    print_case("static_proxy_headers", run_case(operations, [&] {
                   auto result = fiber::access_server::prepare_proxy_response_headers(static_proxy_headers, {});
                   return result ? header_checksum(*result) : 0;
               }));
    print_case("dynamic_proxy_headers", run_case(operations, [&] {
                   auto result =
                           fiber::access_server::prepare_proxy_response_headers(dynamic_proxy_headers, evaluator());
                   return result ? header_checksum(*result) : 0;
               }));
    std::fprintf(stderr, "checksum=%llu\n", static_cast<unsigned long long>(checksum_sink));
    return 0;
}
