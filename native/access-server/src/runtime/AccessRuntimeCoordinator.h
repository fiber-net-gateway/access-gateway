#ifndef FIBER_ACCESS_SERVER_ACCESS_RUNTIME_COORDINATOR_H
#define FIBER_ACCESS_SERVER_ACCESS_RUNTIME_COORDINATOR_H

#include "AccessServerRuntimeError.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

#include <fiber/async/Task.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/net/TlsOptions.h>

namespace fiber::access_server {

class TlsBootstrapIdentity;

enum class AccessServerRuntimeState : std::uint8_t {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
};

struct AccessControlPlaneReady {
    std::shared_ptr<TlsBootstrapIdentity> tls_bootstrap;
    net::TlsIdentitySelectorOps tls_identity_selector;
};

struct AccessControlPlaneLifecycle {
    using StartFunction =
            async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>> (*)(void *context) noexcept;
    using ShutdownFunction = async::Task<void> (*)(void *context) noexcept;

    void *context = nullptr;
    StartFunction start = nullptr;
    ShutdownFunction shutdown = nullptr;
};

struct AccessDataPlaneLifecycle {
    using StartFunction = async::Task<std::expected<void, AccessServerRuntimeError>> (*)(
            void *context, AccessControlPlaneReady ready) noexcept;
    using ShutdownFunction = async::Task<void> (*)(void *context) noexcept;

    void *context = nullptr;
    StartFunction start = nullptr;
    ShutdownFunction shutdown = nullptr;
};

class AccessRuntimeCoordinator final : public common::NonCopyable, public common::NonMovable {
public:
    AccessRuntimeCoordinator(AccessControlPlaneLifecycle control_plane, AccessDataPlaneLifecycle data_plane) noexcept;
    ~AccessRuntimeCoordinator() noexcept;

    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AccessServerRuntimeState state() const noexcept { return state_; }

private:
    AccessControlPlaneLifecycle control_plane_;
    AccessDataPlaneLifecycle data_plane_;
    async::Watch<bool> shutdown_complete_{false};
    std::optional<async::Watch<bool>::Publisher> shutdown_publisher_;
    AccessServerRuntimeState state_ = AccessServerRuntimeState::Created;
    bool data_plane_start_attempted_ = false;
    bool control_plane_shutdown_required_ = true;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_RUNTIME_COORDINATOR_H
