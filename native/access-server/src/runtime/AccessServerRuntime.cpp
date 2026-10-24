#include "AccessServerRuntime.h"

#include "AccessControlPlaneSupervisor.h"
#include "AccessDataPlaneService.h"
#include "AccessRuntimeFactory.h"

#include <new>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

std::expected<std::unique_ptr<AccessServerRuntime>, AccessServerRuntimeError>
AccessServerRuntime::create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
                            event::EventLoopGroup &http_workers, const AccessServerConfig &config,
                            const net::ListenOptions &listen_options, AccessProcessMetricsSources process_metrics) {
    auto components = AccessRuntimeFactory::create(accept_loop, nacos_loop, cat_loop, http_workers, config,
                                                   listen_options, process_metrics);
    if (!components) {
        return std::unexpected(std::move(components.error()));
    }

    auto runtime = std::unique_ptr<AccessServerRuntime>(new (std::nothrow) AccessServerRuntime(
            std::move(components->control_plane), std::move(components->data_plane)));
    if (!runtime) {
        return std::unexpected(AccessServerRuntimeError{
                .code = AccessServerRuntimeErrorCode::AllocateRuntime,
                .create_error = nacos::NacosCreateErrorCode::NoMem,
        });
    }
    return runtime;
}

AccessServerRuntime::AccessServerRuntime(std::unique_ptr<AccessControlPlaneSupervisor> control_plane,
                                         std::unique_ptr<AccessDataPlaneService> data_plane) noexcept :
    control_plane_(std::move(control_plane)), data_plane_(std::move(data_plane)),
    coordinator_(control_plane_->lifecycle(), data_plane_->lifecycle()) {
    FIBER_ASSERT(control_plane_);
    FIBER_ASSERT(data_plane_);
}

AccessServerRuntime::~AccessServerRuntime() noexcept = default;

async::Task<std::expected<void, AccessServerRuntimeError>> AccessServerRuntime::start() noexcept {
    return coordinator_.start();
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessServerRuntime::prepare() noexcept {
    return coordinator_.prepare();
}

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessServerRuntime::activate(bool background_registration) noexcept {
    return coordinator_.activate(background_registration);
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessServerRuntime::release() noexcept {
    return coordinator_.release();
}

bool AccessServerRuntime::upgrade_ready() noexcept { return control_plane_->upgrade_ready(); }

std::string_view AccessServerRuntime::registration_state() const noexcept {
    return control_plane_->registration_state();
}

async::Task<void> AccessServerRuntime::shutdown() noexcept { return coordinator_.shutdown(); }

int AccessServerRuntime::fd() const noexcept { return data_plane_->fd(); }

int AccessServerRuntime::plain_fd() const noexcept { return data_plane_->plain_fd(); }

int AccessServerRuntime::metrics_fd() const noexcept { return data_plane_->metrics_fd(); }

} // namespace fiber::access_server
