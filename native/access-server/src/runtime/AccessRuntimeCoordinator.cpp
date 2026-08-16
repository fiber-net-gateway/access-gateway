#include "AccessRuntimeCoordinator.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

AccessRuntimeCoordinator::AccessRuntimeCoordinator(AccessControlPlaneLifecycle control_plane,
                                                   AccessDataPlaneLifecycle data_plane) noexcept :
    control_plane_(control_plane), data_plane_(data_plane),
    shutdown_publisher_(shutdown_complete_.acquire_publisher()) {
    FIBER_ASSERT(control_plane_.start);
    FIBER_ASSERT(control_plane_.shutdown);
    FIBER_ASSERT(data_plane_.start);
    FIBER_ASSERT(data_plane_.shutdown);
    FIBER_ASSERT(shutdown_publisher_.has_value());
}

AccessRuntimeCoordinator::~AccessRuntimeCoordinator() noexcept {
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created || state_ == AccessServerRuntimeState::Stopped);
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessRuntimeCoordinator::start() noexcept {
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created);
    state_ = AccessServerRuntimeState::Starting;

    auto control_started = co_await control_plane_.start(control_plane_.context);
    if (!control_started) {
        AccessServerRuntimeError error = std::move(control_started.error());
        co_await shutdown();
        co_return std::unexpected(std::move(error));
    }

    data_plane_start_attempted_ = true;
    auto data_started = co_await data_plane_.start(data_plane_.context, std::move(*control_started));
    if (!data_started) {
        AccessServerRuntimeError error = std::move(data_started.error());
        co_await shutdown();
        co_return std::unexpected(std::move(error));
    }

    state_ = AccessServerRuntimeState::Running;
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<void> AccessRuntimeCoordinator::shutdown() noexcept {
    if (state_ == AccessServerRuntimeState::Stopped) {
        co_return;
    }

    auto stopped = shutdown_complete_.subscribe();
    auto snapshot = stopped.current();
    if (state_ == AccessServerRuntimeState::Stopping) {
        while (!snapshot.value || !*snapshot.value) {
            snapshot = co_await stopped.next(snapshot.version);
        }
        co_return;
    }

    state_ = AccessServerRuntimeState::Stopping;
    if (data_plane_start_attempted_) {
        co_await data_plane_.shutdown(data_plane_.context);
        data_plane_start_attempted_ = false;
    }
    if (control_plane_shutdown_required_) {
        co_await control_plane_.shutdown(control_plane_.context);
        control_plane_shutdown_required_ = false;
    }
    state_ = AccessServerRuntimeState::Stopped;
    shutdown_publisher_->publish(true);
}

} // namespace fiber::access_server
