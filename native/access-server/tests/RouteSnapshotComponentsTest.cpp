#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>

#include "routing/AccessRouteSnapshot.h"
#include "routing/ProjectConfigCompiler.h"
#include "runtime/ProjectSnapshotRegistry.h"
#include "runtime/RouteSnapshotPublisher.h"

namespace {

using fiber::access_server::AccessRouteSnapshot;
using fiber::access_server::BodyType;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ProjectConfigCompiler;
using fiber::access_server::ProjectRouteSnapshot;
using fiber::access_server::ProjectSnapshotRegistry;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteSnapshotPublisher;
using fiber::access_server::RouteType;

std::shared_ptr<const ProjectRouteSnapshot> project_snapshot(std::string project, std::int32_t version,
                                                             std::string host) {
    RouteConfig route;
    route.path = "/";
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Text,
            .content = "ok",
    };

    ProjectConfig config;
    config.version = version;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = std::move(host),
                    .strategy = HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    auto compiled = ProjectConfigCompiler{}.compile(project, config);
    if (!compiled || !*compiled) {
        return {};
    }
    return std::make_shared<const ProjectRouteSnapshot>(std::move(**compiled));
}

std::shared_ptr<const AccessRouteSnapshot> access_snapshot(std::string project, std::int32_t version,
                                                           std::string host) {
    auto compiled = project_snapshot(std::move(project), version, std::move(host));
    if (!compiled) {
        return {};
    }
    const std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects{std::move(compiled)};
    auto built = AccessRouteSnapshot::build(projects);
    if (!built) {
        return {};
    }
    return std::make_shared<const AccessRouteSnapshot>(std::move(*built));
}

std::shared_ptr<const AccessRouteSnapshot> pin_on_worker(fiber::event::EventLoopGroup &workers,
                                                         std::size_t worker_index,
                                                         const RouteSnapshotPublisher &publisher) {
    std::promise<std::shared_ptr<const AccessRouteSnapshot>> promise;
    auto future = promise.get_future();
    fiber::async::spawn(workers.at(worker_index), [&]() -> fiber::async::DetachedTask {
        promise.set_value(publisher.pin());
        co_return;
    });
    return future.get();
}

template<typename T, typename U>
bool shares_owner(const std::shared_ptr<T> &left, const std::shared_ptr<U> &right) noexcept {
    return !left.owner_before(right) && !right.owner_before(left);
}

TEST(RouteSnapshotComponentsTest, KeepsConcreteControlPlaneComponentsBounded) {
    EXPECT_FALSE(std::is_polymorphic_v<ProjectConfigCompiler>);
    EXPECT_FALSE(std::is_polymorphic_v<ProjectSnapshotRegistry>);
    EXPECT_FALSE(std::is_polymorphic_v<RouteSnapshotPublisher>);
    EXPECT_LE(sizeof(ProjectConfigCompiler), 32U);
    EXPECT_LE(sizeof(ProjectSnapshotRegistry), 64U);
    EXPECT_LE(sizeof(RouteSnapshotPublisher), 32U);
}

TEST(RouteSnapshotComponentsTest, RegistryOwnsVersionUnloadRemoveAndOrderingSemantics) {
    ProjectSnapshotRegistry registry;
    auto beta = project_snapshot("beta", 1, "beta.example.com");
    auto alpha = project_snapshot("alpha", 2, "alpha.example.com");
    ASSERT_TRUE(beta);
    ASSERT_TRUE(alpha);

    registry.replace("beta", 1, beta);
    registry.replace("alpha", 2, alpha);
    EXPECT_EQ(registry.current_version("alpha"), 2);
    EXPECT_EQ(registry.current_version("beta"), 1);
    auto loaded = registry.loaded_snapshots();
    ASSERT_EQ(loaded.size(), 2U);
    EXPECT_EQ(loaded[0]->project(), "alpha");
    EXPECT_EQ(loaded[1]->project(), "beta");

    registry.unload("alpha");
    EXPECT_EQ(registry.current_version("alpha"), 2);
    EXPECT_EQ(registry.find_snapshot("alpha"), nullptr);
    loaded = registry.loaded_snapshots();
    ASSERT_EQ(loaded.size(), 1U);
    EXPECT_EQ(loaded[0]->project(), "beta");

    registry.remove("alpha");
    EXPECT_FALSE(registry.current_version("alpha"));
    EXPECT_EQ(registry.find_snapshot("alpha"), nullptr);
    registry.clear();
    EXPECT_TRUE(registry.empty());
}

TEST(RouteSnapshotComponentsTest, PublisherKeepsOldPinsAliveAcrossReleasePublication) {
    RouteSnapshotPublisher publisher;
    const auto initial = publisher.pin();
    ASSERT_TRUE(initial);
    EXPECT_TRUE(initial->projects().empty());

    auto project = project_snapshot("demo", 1, "api.example.com");
    ASSERT_TRUE(project);
    const std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects{project};
    auto built = AccessRouteSnapshot::build(projects);
    ASSERT_TRUE(built);
    auto published = std::make_shared<const AccessRouteSnapshot>(std::move(*built));
    publisher.publish(published);

    const auto current = publisher.pin();
    EXPECT_EQ(current, published);
    EXPECT_TRUE(current->match_host("api.example.com"));
    EXPECT_TRUE(initial->projects().empty());

    publisher.publish(std::make_shared<const AccessRouteSnapshot>());
    EXPECT_TRUE(publisher.pin()->projects().empty());
    EXPECT_TRUE(current->match_host("api.example.com"));
}

TEST(RouteSnapshotComponentsTest, PublisherShardsPinOwnersAcrossServingWorkers) {
    fiber::event::EventLoopGroup workers(2);
    RouteSnapshotPublisher publisher(workers);
    auto first = access_snapshot("first", 1, "first.example.com");
    ASSERT_TRUE(first);
    std::weak_ptr<const AccessRouteSnapshot> first_lifetime = first;
    publisher.publish(first);
    auto canonical = publisher.pin();

    workers.start();
    auto worker_zero = pin_on_worker(workers, 0, publisher);
    auto worker_zero_again = pin_on_worker(workers, 0, publisher);
    auto worker_one = pin_on_worker(workers, 1, publisher);
    EXPECT_EQ(worker_zero.get(), canonical.get());
    EXPECT_EQ(worker_one.get(), canonical.get());
    EXPECT_TRUE(shares_owner(worker_zero, worker_zero_again));
    EXPECT_FALSE(shares_owner(worker_zero, worker_one));
    EXPECT_FALSE(shares_owner(worker_zero, canonical));

    auto second = access_snapshot("second", 2, "second.example.com");
    ASSERT_TRUE(second);
    publisher.publish(second);
    auto current_zero = pin_on_worker(workers, 0, publisher);
    auto current_one = pin_on_worker(workers, 1, publisher);
    EXPECT_EQ(current_zero.get(), second.get());
    EXPECT_EQ(current_one.get(), second.get());
    EXPECT_TRUE(current_zero->match_host("second.example.com"));
    EXPECT_TRUE(worker_zero->match_host("first.example.com"));

    first.reset();
    canonical.reset();
    EXPECT_FALSE(first_lifetime.expired());
    worker_zero.reset();
    worker_zero_again.reset();
    worker_one.reset();
    EXPECT_TRUE(first_lifetime.expired());

    workers.stop();
    workers.join();
}

TEST(RouteSnapshotComponentsTest, PublisherSupportsConcurrentWorkerPinsAndHotUpdates) {
    constexpr std::size_t worker_count = 2;
    constexpr std::size_t update_count = 2048;
    fiber::event::EventLoopGroup workers(worker_count);
    RouteSnapshotPublisher publisher(workers);
    auto first = access_snapshot("first", 1, "first.example.com");
    auto second = access_snapshot("second", 2, "second.example.com");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    publisher.publish(first);

    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::vector<std::uint64_t> pin_counts(worker_count);
    workers.start();
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        fiber::async::spawn(workers.at(worker), [&, worker]() -> fiber::async::DetachedTask {
            const auto initial = publisher.pin();
            if (initial.get() != first.get() || !initial->match_host("first.example.com")) {
                failed.store(true, std::memory_order_relaxed);
            }
            std::uint64_t pins = 1;
            ready.fetch_add(1, std::memory_order_release);
            ready.notify_all();
            while (!stop.load(std::memory_order_acquire)) {
                const auto snapshot = publisher.pin();
                const bool is_first = snapshot.get() == first.get();
                const bool is_second = snapshot.get() == second.get();
                const bool valid_first = is_first && snapshot->match_host("first.example.com") &&
                                         !snapshot->match_host("second.example.com");
                const bool valid_second = is_second && snapshot->match_host("second.example.com") &&
                                          !snapshot->match_host("first.example.com");
                if (!valid_first && !valid_second) {
                    failed.store(true, std::memory_order_relaxed);
                }
                ++pins;
            }
            pin_counts[worker] = pins;
            done.fetch_add(1, std::memory_order_release);
            done.notify_all();
            co_return;
        });
    }

    std::size_t ready_count = ready.load(std::memory_order_acquire);
    while (ready_count != worker_count) {
        ready.wait(ready_count, std::memory_order_acquire);
        ready_count = ready.load(std::memory_order_acquire);
    }
    for (std::size_t update = 0; update < update_count; ++update) {
        publisher.publish((update & 1U) == 0 ? second : first);
    }
    stop.store(true, std::memory_order_release);

    std::size_t done_count = done.load(std::memory_order_acquire);
    while (done_count != worker_count) {
        done.wait(done_count, std::memory_order_acquire);
        done_count = done.load(std::memory_order_acquire);
    }
    workers.stop();
    workers.join();

    EXPECT_FALSE(failed.load(std::memory_order_relaxed));
    for (const std::uint64_t pins: pin_counts) {
        EXPECT_GT(pins, 0U);
    }
}

TEST(RouteSnapshotComponentsTest, PublisherFallsBackToCanonicalOutsideServingWorkerGroup) {
    fiber::event::EventLoopGroup serving_workers(1);
    fiber::event::EventLoopGroup other_workers(1);
    RouteSnapshotPublisher publisher(serving_workers);
    auto snapshot = access_snapshot("demo", 1, "api.example.com");
    ASSERT_TRUE(snapshot);
    publisher.publish(snapshot);
    const auto canonical = publisher.pin();

    other_workers.start();
    const auto wrong_group = pin_on_worker(other_workers, 0, publisher);
    other_workers.stop();
    other_workers.join();

    EXPECT_EQ(wrong_group.get(), canonical.get());
    EXPECT_TRUE(shares_owner(wrong_group, canonical));
}

} // namespace
