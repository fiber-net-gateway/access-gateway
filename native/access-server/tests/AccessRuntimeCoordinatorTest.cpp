#include <gtest/gtest.h>

#include <expected>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoop.h>

#include "runtime/AccessRuntimeCoordinator.h"
#include "runtime/AccessServerRuntime.h"

namespace {

using fiber::access_server::AccessControlPlaneLifecycle;
using fiber::access_server::AccessControlPlaneReady;
using fiber::access_server::AccessDataPlaneLifecycle;
using fiber::access_server::AccessRuntimeCoordinator;
using fiber::access_server::AccessServerRuntimeError;
using fiber::access_server::AccessServerRuntimeErrorCode;
using fiber::access_server::AccessServerRuntimeState;

enum class LifecycleEvent {
    ControlStart,
    DataStart,
    DataShutdown,
    ControlShutdown,
};

struct FakeControlPlane {
    std::vector<LifecycleEvent> *events = nullptr;
    std::optional<AccessServerRuntimeError> start_error;
    bool yield_shutdown = false;

    [[nodiscard]] AccessControlPlaneLifecycle lifecycle() noexcept {
        return AccessControlPlaneLifecycle{
                .context = this,
                .start = &start,
                .shutdown = &shutdown,
        };
    }

    [[nodiscard]] static fiber::async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>>
    start(void *context) noexcept {
        auto &self = *static_cast<FakeControlPlane *>(context);
        self.events->push_back(LifecycleEvent::ControlStart);
        if (self.start_error) {
            co_return std::unexpected(*self.start_error);
        }
        co_return AccessControlPlaneReady{};
    }

    [[nodiscard]] static fiber::async::Task<void> shutdown(void *context) noexcept {
        auto &self = *static_cast<FakeControlPlane *>(context);
        self.events->push_back(LifecycleEvent::ControlShutdown);
        if (self.yield_shutdown) {
            co_await fiber::async::yield();
        }
    }
};

struct FakeDataPlane {
    std::vector<LifecycleEvent> *events = nullptr;
    std::optional<AccessServerRuntimeError> start_error;
    bool yield_shutdown = false;

    [[nodiscard]] AccessDataPlaneLifecycle lifecycle() noexcept {
        return AccessDataPlaneLifecycle{
                .context = this,
                .start = &start,
                .shutdown = &shutdown,
        };
    }

    [[nodiscard]] static fiber::async::Task<std::expected<void, AccessServerRuntimeError>>
    start(void *context, AccessControlPlaneReady) noexcept {
        auto &self = *static_cast<FakeDataPlane *>(context);
        self.events->push_back(LifecycleEvent::DataStart);
        if (self.start_error) {
            co_return std::unexpected(*self.start_error);
        }
        co_return std::expected<void, AccessServerRuntimeError>{};
    }

    [[nodiscard]] static fiber::async::Task<void> shutdown(void *context) noexcept {
        auto &self = *static_cast<FakeDataPlane *>(context);
        self.events->push_back(LifecycleEvent::DataShutdown);
        if (self.yield_shutdown) {
            co_await fiber::async::yield();
        }
    }
};

template<typename Function>
void run_on_loop(Function function) {
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        co_await function();
        loop.stop();
    });
    loop.run();
}

TEST(AccessRuntimeCoordinatorTest, KeepsPublicFacadeSmallAndLifecycleAdaptersConcrete) {
    EXPECT_LE(sizeof(fiber::access_server::AccessServerRuntime), 128U);
    EXPECT_FALSE(std::is_polymorphic_v<fiber::access_server::AccessServerRuntime>);
    EXPECT_FALSE(std::is_polymorphic_v<AccessRuntimeCoordinator>);
    EXPECT_TRUE(std::is_trivially_copyable_v<AccessControlPlaneLifecycle>);
    EXPECT_TRUE(std::is_trivially_copyable_v<AccessDataPlaneLifecycle>);
}

TEST(AccessRuntimeCoordinatorTest, StartsInOrderAndCoalescesConcurrentShutdown) {
    std::vector<LifecycleEvent> events;
    FakeControlPlane control{.events = &events, .yield_shutdown = true};
    FakeDataPlane data{.events = &events, .yield_shutdown = true};

    run_on_loop([&]() -> fiber::async::Task<void> {
        AccessRuntimeCoordinator coordinator(control.lifecycle(), data.lifecycle());
        EXPECT_EQ(coordinator.state(), AccessServerRuntimeState::Created);

        auto started = co_await coordinator.start();
        EXPECT_TRUE(started);
        if (!started) {
            co_return;
        }
        EXPECT_EQ(coordinator.state(), AccessServerRuntimeState::Running);

        fiber::async::WaitGroup shutdowns;
        shutdowns.add(2);
        fiber::async::spawn([&]() -> fiber::async::DetachedTask {
            co_await coordinator.shutdown();
            shutdowns.done();
        });
        fiber::async::spawn([&]() -> fiber::async::DetachedTask {
            co_await coordinator.shutdown();
            shutdowns.done();
        });
        co_await shutdowns.join();
        co_await coordinator.shutdown();
        EXPECT_EQ(coordinator.state(), AccessServerRuntimeState::Stopped);
    });

    EXPECT_EQ(events, (std::vector<LifecycleEvent>{LifecycleEvent::ControlStart, LifecycleEvent::DataStart,
                                                   LifecycleEvent::DataShutdown, LifecycleEvent::ControlShutdown}));
}

TEST(AccessRuntimeCoordinatorTest, RollsBackControlPlaneFailureWithoutStartingDataPlane) {
    std::vector<LifecycleEvent> events;
    FakeControlPlane control{
            .events = &events,
            .start_error = AccessServerRuntimeError{.code = AccessServerRuntimeErrorCode::StartNacosClient},
    };
    FakeDataPlane data{.events = &events};

    run_on_loop([&]() -> fiber::async::Task<void> {
        AccessRuntimeCoordinator coordinator(control.lifecycle(), data.lifecycle());
        auto started = co_await coordinator.start();
        EXPECT_FALSE(started);
        if (started) {
            co_await coordinator.shutdown();
            co_return;
        }
        EXPECT_EQ(started.error().code, AccessServerRuntimeErrorCode::StartNacosClient);
        EXPECT_EQ(coordinator.state(), AccessServerRuntimeState::Stopped);
    });

    EXPECT_EQ(events, (std::vector<LifecycleEvent>{LifecycleEvent::ControlStart, LifecycleEvent::ControlShutdown}));
}

TEST(AccessRuntimeCoordinatorTest, RollsBackDataPlaneFailureInReverseOrder) {
    std::vector<LifecycleEvent> events;
    FakeControlPlane control{.events = &events};
    FakeDataPlane data{
            .events = &events,
            .start_error = AccessServerRuntimeError{.code = AccessServerRuntimeErrorCode::BindMetrics},
    };

    run_on_loop([&]() -> fiber::async::Task<void> {
        AccessRuntimeCoordinator coordinator(control.lifecycle(), data.lifecycle());
        auto started = co_await coordinator.start();
        EXPECT_FALSE(started);
        if (started) {
            co_await coordinator.shutdown();
            co_return;
        }
        EXPECT_EQ(started.error().code, AccessServerRuntimeErrorCode::BindMetrics);
        EXPECT_EQ(coordinator.state(), AccessServerRuntimeState::Stopped);
    });

    EXPECT_EQ(events, (std::vector<LifecycleEvent>{LifecycleEvent::ControlStart, LifecycleEvent::DataStart,
                                                   LifecycleEvent::DataShutdown, LifecycleEvent::ControlShutdown}));
}

TEST(AccessRuntimeCoordinatorTest, ShutdownBeforeStartReleasesOwnedControlPlaneOnly) {
    std::vector<LifecycleEvent> events;
    FakeControlPlane control{.events = &events};
    FakeDataPlane data{.events = &events};

    run_on_loop([&]() -> fiber::async::Task<void> {
        AccessRuntimeCoordinator coordinator(control.lifecycle(), data.lifecycle());
        co_await coordinator.shutdown();
        EXPECT_EQ(coordinator.state(), AccessServerRuntimeState::Stopped);
    });

    EXPECT_EQ(events, (std::vector<LifecycleEvent>{LifecycleEvent::ControlShutdown}));
}

} // namespace
