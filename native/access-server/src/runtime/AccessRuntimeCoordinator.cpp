#include "AccessRuntimeCoordinator.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

AccessRuntimeCoordinator::AccessRuntimeCoordinator(AccessControlPlaneLifecycle control_plane,
                                                   AccessDataPlaneLifecycle data_plane) noexcept :
    control_plane_(control_plane), data_plane_(data_plane),
    shutdown_publisher_(shutdown_complete_.acquire_publisher()) {
    FIBER_ASSERT(control_plane_.start);
    FIBER_ASSERT(control_plane_.register_instance);
    FIBER_ASSERT(control_plane_.deregister_instance);
    FIBER_ASSERT(control_plane_.shutdown);
    FIBER_ASSERT(data_plane_.prepare);
    FIBER_ASSERT(data_plane_.bind);
    FIBER_ASSERT(data_plane_.begin_drain);
    FIBER_ASSERT(data_plane_.serve);
    FIBER_ASSERT(data_plane_.shutdown);
    FIBER_ASSERT(shutdown_publisher_.has_value());
}

AccessRuntimeCoordinator::~AccessRuntimeCoordinator() noexcept {
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created || state_ == AccessServerRuntimeState::Stopped);
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessRuntimeCoordinator::start() noexcept {
    auto prepared = co_await prepare();
    if (!prepared) {
        co_return std::unexpected(std::move(prepared.error()));
    }
    co_return co_await activate(false);
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessRuntimeCoordinator::prepare() noexcept {
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created);
    state_ = AccessServerRuntimeState::Starting;
    auto control = co_await control_plane_.start(control_plane_.context);
    if (!control) {
        auto error = std::move(control.error());
        co_await shutdown();
        co_return std::unexpected(std::move(error));
    }
    // Also clean up a partially initialized, never-bound data plane.
    data_plane_bind_attempted_ = true;
    auto prepared = co_await data_plane_.prepare(data_plane_.context, std::move(*control));
    if (!prepared) {
        auto error = std::move(prepared.error());
        co_await shutdown();
        co_return std::unexpected(std::move(error));
    }
    state_ = AccessServerRuntimeState::Prepared;
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessRuntimeCoordinator::activate(bool background_registration) noexcept {
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Prepared);
    state_ = AccessServerRuntimeState::Activating;
    auto bound = co_await data_plane_.bind(data_plane_.context);
    if (!bound) {
        auto error = std::move(bound.error());
        co_await shutdown();
        co_return std::unexpected(std::move(error));
    }
    if (!background_registration) {
        registration_attempted_ = true;
        auto registered = co_await control_plane_.register_instance(control_plane_.context, *bound);
        if (!registered) {
            auto error = std::move(registered.error());
            co_await shutdown();
            co_return std::unexpected(std::move(error));
        }
    }
    auto serving = co_await data_plane_.serve(data_plane_.context);
    if (!serving) {
        auto error = std::move(serving.error());
        co_await shutdown();
        co_return std::unexpected(std::move(error));
    }
    state_ = AccessServerRuntimeState::Running;
    if (background_registration) {
        FIBER_ASSERT(control_plane_.register_background);
        registration_attempted_ = true;
        control_plane_.register_background(control_plane_.context, *bound);
    }
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessRuntimeCoordinator::release() noexcept {
    if (state_ == AccessServerRuntimeState::Draining) {
        if (!listeners_released_) {
            co_return std::unexpected(
                    make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::Serve, common::IoErr::Busy));
        }
        co_return std::expected<void, AccessServerRuntimeError>{};
    }
    if (state_ != AccessServerRuntimeState::Running) {
        co_return std::unexpected(
                make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::Serve, common::IoErr::Invalid));
    }
    state_ = AccessServerRuntimeState::Draining;
    if (registration_attempted_) {
        // Local owner-loop cancellation only. Remote deregistration is best effort.
        co_await control_plane_.deregister_instance(control_plane_.context);
        registration_attempted_ = false;
    }
    data_plane_.begin_drain(data_plane_.context);
    listeners_released_ = true;
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<void> AccessRuntimeCoordinator::shutdown() noexcept {
    if (state_ == AccessServerRuntimeState::Stopped) {
        co_return;
    }

    auto stopped = shutdown_complete_.subscribe();
    auto snapshot = stopped.current();
    if (shutdown_started_) {
        while (!snapshot.value || !*snapshot.value) {
            snapshot = co_await stopped.next(snapshot.version);
        }
        co_return;
    }

    shutdown_started_ = true;
    if (state_ != AccessServerRuntimeState::Draining) {
        state_ = AccessServerRuntimeState::Stopping;
    }
    if (registration_attempted_) {
        co_await control_plane_.deregister_instance(control_plane_.context);
        registration_attempted_ = false;
    }
    if (data_plane_bind_attempted_) {
        co_await data_plane_.shutdown(data_plane_.context);
        data_plane_bind_attempted_ = false;
    }
    state_ = AccessServerRuntimeState::Stopping;
    if (control_plane_shutdown_required_) {
        co_await control_plane_.shutdown(control_plane_.context);
        control_plane_shutdown_required_ = false;
    }
    state_ = AccessServerRuntimeState::Stopped;
    shutdown_publisher_->publish(true);
}

} // namespace fiber::access_server
