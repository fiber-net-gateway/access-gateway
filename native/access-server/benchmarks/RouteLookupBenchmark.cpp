#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/common/util/CpuConcurrency.h>
#include <fiber/event/EventLoopGroup.h>

#include "config/AccessConfig.h"
#include "routing/AccessRouteSnapshot.h"
#include "routing/ProjectConfigCompiler.h"
#include "runtime/RouteSnapshotPublisher.h"

namespace {

using fiber::access_server::AccessRouteSnapshot;
using fiber::access_server::BodyType;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::PathVariable;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ProjectConfigCompiler;
using fiber::access_server::ProjectRouteSnapshot;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteSnapshotPublisher;
using fiber::access_server::RouteType;

constexpr std::size_t kProjectCount = 352;
constexpr std::size_t kQueryCount = 4096;
constexpr std::size_t kSamples = 21;
constexpr std::uint64_t kDefaultOperationsPerSample = 100'000;

enum class CaseKind : std::uint8_t {
    GlobalPinOnly,
    WorkerPinOnly,
    PinnedLookup,
    GlobalPinAndLookup,
    WorkerPinAndLookup,
};

struct Query {
    std::string host;
    std::string path;
    std::uint64_t expected = 0;
};

class PathMatchContext {
public:
    bool matched(std::uint32_t, std::uint32_t route_index) noexcept {
        route_index_ = route_index;
        matched_variable_count_ = variable_count_;
        return true;
    }

    void add_path_var(std::string_view name, std::string_view value) noexcept {
        if (variable_count_ == variables_.size()) {
            std::abort();
        }
        variables_[variable_count_++] = PathVariable{
                .name = name,
                .value = value,
        };
    }

    void pop_path_var() noexcept {
        if (variable_count_ == 0) {
            std::abort();
        }
        --variable_count_;
    }

    [[nodiscard]] std::uint64_t checksum() const noexcept {
        if (route_index_ == std::numeric_limits<std::uint32_t>::max()) {
            return 2;
        }
        std::uint64_t result = static_cast<std::uint64_t>(route_index_) + 3U;
        for (std::size_t index = 0; index < matched_variable_count_; ++index) {
            result = result * 131U + variables_[index].name.size();
            result = result * 131U + variables_[index].value.size();
        }
        return result;
    }

private:
    std::array<PathVariable, 2> variables_{};
    std::size_t variable_count_ = 0;
    std::size_t matched_variable_count_ = 0;
    std::uint32_t route_index_ = std::numeric_limits<std::uint32_t>::max();
};

RouteConfig response_route(std::string path) {
    RouteConfig route;
    route.path = std::move(path);
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Text,
            .content = "ok",
    };
    return route;
}

ProjectConfig project_config(std::size_t index) {
    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = "project-" + std::to_string(index) + ".benchmark.example",
                    .strategy = HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<RouteConfig>>{};
    config.routes->reserve(3);
    config.routes->emplace_back(response_route("/health"));
    config.routes->emplace_back(response_route("/items/:id"));
    config.routes->emplace_back(response_route("/orders/:order/items/:item"));
    return config;
}

std::shared_ptr<const AccessRouteSnapshot> build_snapshot() {
    ProjectConfigCompiler compiler;
    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects;
    projects.reserve(kProjectCount);
    for (std::size_t index = 0; index < kProjectCount; ++index) {
        const std::string project = "project-" + std::to_string(index);
        auto compiled = compiler.compile(project, project_config(index));
        if (!compiled || !*compiled) {
            std::abort();
        }
        projects.push_back(std::make_shared<const ProjectRouteSnapshot>(std::move(**compiled)));
    }
    auto built = AccessRouteSnapshot::build(projects);
    if (!built) {
        std::abort();
    }
    return std::make_shared<const AccessRouteSnapshot>(std::move(*built));
}

std::uint64_t lookup(const AccessRouteSnapshot &snapshot, const Query &query) noexcept {
    const fiber::access_server::ProjectHostMatch host_match = snapshot.match_host(query.host);
    if (!host_match) {
        return 1;
    }
    PathMatchContext context;
    if (!host_match.project->match_route_path(query.path, context)) {
        return 2;
    }
    return context.checksum();
}

std::vector<Query> build_queries(const AccessRouteSnapshot &snapshot) {
    std::vector<Query> queries;
    queries.reserve(kQueryCount);
    std::uint32_t random = 0x9e3779b9U;
    for (std::size_t index = 0; index < kQueryCount; ++index) {
        random = random * 1664525U + 1013904223U;
        const std::size_t project = random % kProjectCount;
        Query query;
        if ((index & 7U) == 0) {
            query.host = "missing-" + std::to_string(project) + ".benchmark.example";
            query.path = "/health";
        } else {
            query.host = "PROJECT-" + std::to_string(project) + ".BENCHMARK.EXAMPLE";
            switch (index & 7U) {
                case 1:
                    query.path = "/missing/" + std::to_string(random);
                    break;
                case 2:
                case 3:
                    query.path = "/health";
                    break;
                case 4:
                case 5:
                    query.path = "/items/" + std::to_string(random);
                    break;
                default:
                    query.path = "/orders/" + std::to_string(random) + "/items/" + std::to_string(index);
                    break;
            }
        }
        query.expected = lookup(snapshot, query);
        queries.push_back(std::move(query));
    }
    return queries;
}

class BenchmarkFixture {
public:
    explicit BenchmarkFixture(fiber::event::EventLoopGroup &workers) :
        worker_publisher_(workers), snapshot_(build_snapshot()), queries_(build_queries(*snapshot_)) {
        global_publisher_.publish(snapshot_);
        worker_publisher_.publish(snapshot_);
    }

    [[nodiscard]] const RouteSnapshotPublisher &global_publisher() const noexcept { return global_publisher_; }
    [[nodiscard]] const RouteSnapshotPublisher &worker_publisher() const noexcept { return worker_publisher_; }
    [[nodiscard]] const std::shared_ptr<const AccessRouteSnapshot> &snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const std::vector<Query> &queries() const noexcept { return queries_; }

private:
    RouteSnapshotPublisher global_publisher_;
    RouteSnapshotPublisher worker_publisher_;
    std::shared_ptr<const AccessRouteSnapshot> snapshot_;
    std::vector<Query> queries_;
};

struct CaseState {
    CaseState(const BenchmarkFixture &value_fixture, CaseKind value_kind, std::size_t value_workers,
              std::uint64_t value_operations) :
        fixture(&value_fixture), kind(value_kind), workers(value_workers), operations(value_operations),
        checksums(value_workers) {}

    const BenchmarkFixture *fixture = nullptr;
    CaseKind kind = CaseKind::GlobalPinOnly;
    std::size_t workers = 0;
    std::uint64_t operations = 0;
    std::vector<std::uint64_t> checksums;
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
};

struct SampleResult {
    std::uint64_t elapsed_ns = 0;
    std::uint64_t checksum = 0;
    bool failed = false;
};

std::uint64_t run_pin_operations(const RouteSnapshotPublisher &publisher, std::uint64_t operations) noexcept {
    std::uint64_t checksum = 0;
    for (std::uint64_t operation = 0; operation < operations; ++operation) {
        const auto snapshot = publisher.pin();
        checksum += snapshot->projects().size();
    }
    return checksum;
}

template<bool PinEachOperation>
std::uint64_t run_lookup_operations(CaseState &run, std::size_t worker, std::uint64_t operations,
                                    const RouteSnapshotPublisher *publisher,
                                    const std::shared_ptr<const AccessRouteSnapshot> &pinned) noexcept {
    std::uint64_t checksum = 0;
    for (std::uint64_t operation = 0; operation < operations; ++operation) {
        const Query &query = run.fixture->queries()[(operation + worker * 977U) % kQueryCount];
        std::uint64_t result;
        if constexpr (PinEachOperation) {
            const auto snapshot = publisher->pin();
            result = lookup(*snapshot, query);
        } else {
            result = lookup(*pinned, query);
        }
        if (result != query.expected) {
            run.failed.store(true, std::memory_order_relaxed);
        }
        checksum += result;
    }
    return checksum;
}

SampleResult run_sample(fiber::event::EventLoopGroup &group, const BenchmarkFixture &fixture, CaseKind kind,
                        std::size_t worker_count, std::uint64_t operations) {
    auto run = std::make_shared<CaseState>(fixture, kind, worker_count, operations);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        fiber::async::spawn(group.at(worker), [run, worker]() -> fiber::async::DetachedTask {
            std::shared_ptr<const AccessRouteSnapshot> pinned;
            if (run->kind == CaseKind::PinnedLookup) {
                pinned = run->fixture->snapshot();
            }
            run->ready.fetch_add(1, std::memory_order_release);
            run->ready.notify_all();
            while (!run->start.load(std::memory_order_acquire)) {
                co_await fiber::async::yield();
            }

            const std::uint64_t base_operations = run->operations / run->workers;
            const std::uint64_t worker_operations = base_operations + (worker < run->operations % run->workers);
            switch (run->kind) {
                case CaseKind::GlobalPinOnly:
                    run->checksums[worker] = run_pin_operations(run->fixture->global_publisher(), worker_operations);
                    break;
                case CaseKind::WorkerPinOnly:
                    run->checksums[worker] = run_pin_operations(run->fixture->worker_publisher(), worker_operations);
                    break;
                case CaseKind::PinnedLookup:
                    run->checksums[worker] =
                            run_lookup_operations<false>(*run, worker, worker_operations, nullptr, pinned);
                    break;
                case CaseKind::GlobalPinAndLookup:
                    run->checksums[worker] = run_lookup_operations<true>(*run, worker, worker_operations,
                                                                         &run->fixture->global_publisher(), {});
                    break;
                case CaseKind::WorkerPinAndLookup:
                    run->checksums[worker] = run_lookup_operations<true>(*run, worker, worker_operations,
                                                                         &run->fixture->worker_publisher(), {});
                    break;
            }
            run->done.fetch_add(1, std::memory_order_release);
            run->done.notify_all();
            co_return;
        });
    }

    std::size_t ready = run->ready.load(std::memory_order_acquire);
    while (ready != worker_count) {
        run->ready.wait(ready, std::memory_order_acquire);
        ready = run->ready.load(std::memory_order_acquire);
    }
    const auto started = std::chrono::steady_clock::now();
    run->start.store(true, std::memory_order_release);

    std::size_t done = run->done.load(std::memory_order_acquire);
    while (done != worker_count) {
        run->done.wait(done, std::memory_order_acquire);
        done = run->done.load(std::memory_order_acquire);
    }

    SampleResult result;
    result.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    result.failed = run->failed.load(std::memory_order_relaxed);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        result.checksum += run->checksums[worker];
    }
    return result;
}

struct CaseResult {
    double p50_ns_per_operation = 0;
    double p95_ns_per_operation = 0;
    double p99_ns_per_operation = 0;
    double operations_per_second = 0;
    std::uint64_t checksum = 0;
};

std::size_t percentile_index(std::size_t count, std::size_t percentile) noexcept {
    return (count * percentile + 99U) / 100U - 1U;
}

CaseResult measure_case(fiber::event::EventLoopGroup &group, const BenchmarkFixture &fixture, CaseKind kind,
                        std::size_t worker_count, std::uint64_t operations) {
    std::array<std::uint64_t, kSamples> elapsed{};
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < kSamples; ++sample) {
        const SampleResult result = run_sample(group, fixture, kind, worker_count, operations);
        if (result.failed) {
            std::abort();
        }
        elapsed[sample] = result.elapsed_ns;
        checksum ^= result.checksum + sample;
    }
    std::sort(elapsed.begin(), elapsed.end());
    const auto ns_per_operation = [operations](std::uint64_t value) {
        return static_cast<double>(value) / static_cast<double>(operations);
    };
    const std::uint64_t p50 = elapsed[percentile_index(elapsed.size(), 50)];
    return {
            .p50_ns_per_operation = ns_per_operation(p50),
            .p95_ns_per_operation = ns_per_operation(elapsed[percentile_index(elapsed.size(), 95)]),
            .p99_ns_per_operation = ns_per_operation(elapsed[percentile_index(elapsed.size(), 99)]),
            .operations_per_second =
                    p50 == 0 ? 0.0 : static_cast<double>(operations) * 1'000'000'000.0 / static_cast<double>(p50),
            .checksum = checksum,
    };
}

bool parse_positive(std::string_view input, std::uint64_t &value) noexcept {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), parsed);
    if (error != std::errc{} || end != input.data() + input.size() || parsed == 0) {
        return false;
    }
    value = parsed;
    return true;
}

std::string_view case_name(CaseKind kind) noexcept {
    switch (kind) {
        case CaseKind::GlobalPinOnly:
            return "global_pin_only";
        case CaseKind::WorkerPinOnly:
            return "worker_pin_only";
        case CaseKind::PinnedLookup:
            return "pinned_lookup";
        case CaseKind::GlobalPinAndLookup:
            return "global_pin_lookup";
        case CaseKind::WorkerPinAndLookup:
            return "worker_pin_lookup";
    }
    return "unknown";
}

void print_case(CaseKind kind, std::size_t worker_count, std::uint64_t operations, const CaseResult &result) {
    const std::string_view name = case_name(kind);
    std::printf("%.*s,%zu,%llu,%.2f,%.2f,%.2f,%.0f,%llu\n", static_cast<int>(name.size()), name.data(), worker_count,
                static_cast<unsigned long long>(operations), result.p50_ns_per_operation, result.p95_ns_per_operation,
                result.p99_ns_per_operation, result.operations_per_second,
                static_cast<unsigned long long>(result.checksum));
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t operations_per_sample = kDefaultOperationsPerSample;
    const fiber::util::CpuConcurrency cpu = fiber::util::detect_cpu_concurrency();
    const std::size_t detected_cpus = cpu.effective_count;
    std::uint64_t requested_workers = detected_cpus;
    if ((argc >= 2 && !parse_positive(argv[1], operations_per_sample)) ||
        (argc >= 3 && !parse_positive(argv[2], requested_workers)) || argc > 3 ||
        requested_workers > std::numeric_limits<std::size_t>::max()) {
        std::fprintf(stderr, "usage: %s [operations-per-sample] [max-workers]\n", argv[0]);
        return 2;
    }

    const std::size_t max_workers = std::min<std::size_t>(static_cast<std::size_t>(requested_workers), detected_cpus);
    if (operations_per_sample < max_workers) {
        std::fprintf(stderr, "operations-per-sample must be at least max-workers\n");
        return 2;
    }
    const std::string_view cpu_source = fiber::util::cpu_concurrency_source_name(cpu.source);
    std::fprintf(stderr, "projects=%zu routes=%zu queries=%zu samples=%zu effective_cpus=%zu source=%.*s\n",
                 kProjectCount, kProjectCount * 3U, kQueryCount, kSamples, detected_cpus,
                 static_cast<int>(cpu_source.size()), cpu_source.data());

    fiber::event::EventLoopGroup group(max_workers);
    BenchmarkFixture fixture(group);
    group.start();
    std::printf("case,workers,operations,p50_ns_per_operation,p95_ns_per_operation,p99_ns_per_operation,"
                "operations_per_second,checksum\n");
    for (std::size_t workers = 1; workers <= max_workers; ++workers) {
        for (const CaseKind kind: {CaseKind::GlobalPinOnly, CaseKind::WorkerPinOnly, CaseKind::PinnedLookup,
                                   CaseKind::GlobalPinAndLookup, CaseKind::WorkerPinAndLookup}) {
            print_case(kind, workers, operations_per_sample,
                       measure_case(group, fixture, kind, workers, operations_per_sample));
        }
    }
    group.stop();
    group.join();
    return 0;
}
