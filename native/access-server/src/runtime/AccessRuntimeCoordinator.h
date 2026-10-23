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
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsParams.h>

namespace fiber::access_server {

class TlsBootstrapIdentity;

enum class AccessServerRuntimeState : std::uint8_t {
    Created,
    Starting,
    Prepared,
    Activating,
    Running,
    Draining,
    Stopping,
    Stopped,
};

struct AccessControlPlaneReady {
    std::shared_ptr<TlsBootstrapIdentity> tls_bootstrap;
    net::ConfigureTlsCallback tls_configure_callback = nullptr;
    void *tls_configure_context = nullptr;
};

struct AccessBoundEndpoint {
    net::SocketAddress address;
    bool tls = false;
};

enum class AccessRegistrationListener : std::uint8_t {
    Tls,
    Plain,
};

struct AccessControlPlaneLifecycle {
    using StartFunction =
            async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>> (*)(void *context) noexcept;
    using RegisterFunction = async::Task<std::expected<void, AccessServerRuntimeError>> (*)(
            void *context, AccessBoundEndpoint endpoint) noexcept;
    using ShutdownFunction = async::Task<void> (*)(void *context) noexcept;

    void *context = nullptr;
    StartFunction start = nullptr;
    RegisterFunction register_instance = nullptr;
    void (*register_background)(void *, AccessBoundEndpoint) noexcept = nullptr;
    ShutdownFunction deregister_instance = nullptr;
    ShutdownFunction shutdown = nullptr;
};

struct AccessDataPlaneLifecycle {
    using PrepareFunction = async::Task<std::expected<void, AccessServerRuntimeError>> (*)(
            void *context, AccessControlPlaneReady ready) noexcept;
    using BindFunction = async::Task<std::expected<AccessBoundEndpoint, AccessServerRuntimeError>> (*)(void *) noexcept;
    using ServeFunction = async::Task<std::expected<void, AccessServerRuntimeError>> (*)(void *context) noexcept;
    using ShutdownFunction = async::Task<void> (*)(void *context) noexcept;

    void *context = nullptr;
    PrepareFunction prepare = nullptr;
    BindFunction bind = nullptr;
    void (*begin_drain)(void *) noexcept = nullptr;
    ServeFunction serve = nullptr;
    ShutdownFunction shutdown = nullptr;
};

class AccessRuntimeCoordinator final : public common::NonCopyable, public common::NonMovable {
public:
    AccessRuntimeCoordinator(AccessControlPlaneLifecycle control_plane, AccessDataPlaneLifecycle data_plane) noexcept;
    ~AccessRuntimeCoordinator() noexcept;

    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> start() noexcept;
    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> prepare() noexcept;
    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>>
    activate(bool background_registration) noexcept;
    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> release() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AccessServerRuntimeState state() const noexcept { return state_; }

private:
    AccessControlPlaneLifecycle control_plane_;
    AccessDataPlaneLifecycle data_plane_;
    async::Watch<bool> shutdown_complete_{false};
    std::optional<async::Watch<bool>::Publisher> shutdown_publisher_;
    AccessServerRuntimeState state_ = AccessServerRuntimeState::Created;
    bool data_plane_bind_attempted_ = false;
    bool registration_attempted_ = false;
    bool listeners_released_ = false;
    bool shutdown_started_ = false;
    bool control_plane_shutdown_required_ = true;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_RUNTIME_COORDINATOR_H
