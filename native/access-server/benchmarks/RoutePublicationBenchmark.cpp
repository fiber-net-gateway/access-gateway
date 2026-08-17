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
#include "config/AccessConfig.h"
#include "routing/ProjectConfigCompiler.h"
#include "runtime/RouteConfigStore.h"

namespace {

using fiber::access_server::BodyType;
using fiber::access_server::ConfigUpdateStatus;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ReadyProjectUpdate;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteConfigStore;
using fiber::access_server::RouteType;

constexpr std::array<std::size_t, 4> kProjectCounts{10, 100, 352, 500};

std::uint64_t checksum_sink = 0;

ProjectConfig project_config(std::size_t index) {
    RouteConfig route;
    route.path = "/";
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Text,
            .content = "ok",
    };

    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = "project-" + std::to_string(index) + ".benchmark.example",
                    .strategy = HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    return config;
}

std::vector<ReadyProjectUpdate> prepare_updates(RouteConfigStore &store, std::size_t count) {
    std::vector<ReadyProjectUpdate> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::string project = "project-" + std::to_string(index);
        auto compiled = fiber::access_server::compile_project_config(project, project_config(index));
        if (!compiled) {
            std::abort();
        }
        auto prepared = store.prepare_compiled(project, 1, std::move(*compiled));
        if (!prepared) {
            std::abort();
        }
        auto ready = std::move(*prepared).try_ready();
        if (!ready) {
            std::abort();
        }
        result.push_back(std::move(*ready));
    }
    return result;
}

std::uint64_t run_sequential(std::size_t count) {
    RouteConfigStore store;
    std::vector<ReadyProjectUpdate> updates = prepare_updates(store, count);
    const auto started = std::chrono::steady_clock::now();
    for (ReadyProjectUpdate &update: updates) {
        auto committed = store.commit(std::move(update));
        if (!committed || committed->status != ConfigUpdateStatus::Published) {
            std::abort();
        }
    }
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
    checksum_sink += store.pin()->host_count();
    return static_cast<std::uint64_t>(elapsed.count());
}

std::uint64_t run_batch(std::size_t count) {
    RouteConfigStore store;
    std::vector<ReadyProjectUpdate> updates = prepare_updates(store, count);
    const auto started = std::chrono::steady_clock::now();
    auto committed = store.commit_batch(std::move(updates));
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
    if (!committed || !committed->published || committed->projects.size() != count) {
        std::abort();
    }
    for (const auto &project: committed->projects) {
        if (!project.outcome || *project.outcome != ConfigUpdateStatus::Published) {
            std::abort();
        }
    }
    checksum_sink += store.pin()->host_count();
    return static_cast<std::uint64_t>(elapsed.count());
}

std::uint64_t run_full_pipeline(std::size_t count) {
    RouteConfigStore store;
    const auto started = std::chrono::steady_clock::now();
    std::vector<ReadyProjectUpdate> updates = prepare_updates(store, count);
    auto committed = store.commit_batch(std::move(updates));
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
    if (!committed || !committed->published || committed->projects.size() != count) {
        std::abort();
    }
    for (const auto &project: committed->projects) {
        if (!project.outcome || *project.outcome != ConfigUpdateStatus::Published) {
            std::abort();
        }
    }
    checksum_sink += store.pin()->host_count();
    return static_cast<std::uint64_t>(elapsed.count());
}

struct Result {
    fiber::access_server::benchmark::Distribution sequential;
    fiber::access_server::benchmark::Distribution batch;
    fiber::access_server::benchmark::Distribution full_pipeline;
};

Result measure(std::size_t count) {
    std::vector<std::uint64_t> sequential;
    std::vector<std::uint64_t> batch;
    std::vector<std::uint64_t> full_pipeline;
    sequential.reserve(fiber::access_server::benchmark::kDefaultSamples);
    batch.reserve(fiber::access_server::benchmark::kDefaultSamples);
    full_pipeline.reserve(fiber::access_server::benchmark::kDefaultSamples);
    for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
        sequential.push_back(run_sequential(count));
        batch.push_back(run_batch(count));
        full_pipeline.push_back(run_full_pipeline(count));
    }
    return {
            .sequential = fiber::access_server::benchmark::summarize(std::move(sequential), 1),
            .batch = fiber::access_server::benchmark::summarize(std::move(batch), 1),
            .full_pipeline = fiber::access_server::benchmark::summarize(std::move(full_pipeline), 1),
    };
}

} // namespace

int main() {
    std::printf("projects,sequential_p50_us,batch_p50_us,full_pipeline_p50_us,full_pipeline_p95_us,"
                "full_pipeline_p99_us,full_pipeline_projects_per_second,speedup,sequential_publications,"
                "batch_publications\n");
    for (const std::size_t count: kProjectCounts) {
        const Result result = measure(count);
        const double sequential_microseconds = result.sequential.p50_ns_per_operation / 1000.0;
        const double batch_microseconds = result.batch.p50_ns_per_operation / 1000.0;
        const double full_pipeline_microseconds = result.full_pipeline.p50_ns_per_operation / 1000.0;
        const double speedup = batch_microseconds == 0.0 ? 0.0 : sequential_microseconds / batch_microseconds;
        const double projects_per_second = result.full_pipeline.operations_per_second * static_cast<double>(count);
        std::printf("%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.2f,%zu,1\n", count, sequential_microseconds,
                    batch_microseconds, full_pipeline_microseconds, result.full_pipeline.p95_ns_per_operation / 1000.0,
                    result.full_pipeline.p99_ns_per_operation / 1000.0, projects_per_second, speedup, count);
    }
    std::fprintf(stderr, "checksum=%llu\n", static_cast<unsigned long long>(checksum_sink));
    return 0;
}
