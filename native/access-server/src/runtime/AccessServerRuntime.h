#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_H

#include "../observability/AccessProcessMetrics.h"
#include "AccessRuntimeCoordinator.h"
#include "AccessServerConfig.h"

#include <expected>
#include <memory>

#include <fiber/async/Task.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/TcpListener.h>

namespace fiber::access_server {

class AccessControlPlaneSupervisor;
class AccessDataPlaneService;

class AccessServerRuntime final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<AccessServerRuntime>, AccessServerRuntimeError>
    create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &compiler_loop,
           event::EventLoop &cat_loop, event::EventLoopGroup &http_workers, const AccessServerConfig &config,
           const net::ListenOptions &listen_options = {}, AccessProcessMetricsSources process_metrics = {});

    ~AccessServerRuntime() noexcept;

    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AccessServerRuntimeState state() const noexcept { return coordinator_.state(); }
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] int metrics_fd() const noexcept;

private:
    AccessServerRuntime(std::unique_ptr<AccessControlPlaneSupervisor> control_plane,
                        std::unique_ptr<AccessDataPlaneService> data_plane) noexcept;

    std::unique_ptr<AccessControlPlaneSupervisor> control_plane_;
    std::unique_ptr<AccessDataPlaneService> data_plane_;
    AccessRuntimeCoordinator coordinator_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_H
