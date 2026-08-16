#ifndef FIBER_ACCESS_SERVER_ACCESS_RUNTIME_FACTORY_H
#define FIBER_ACCESS_SERVER_ACCESS_RUNTIME_FACTORY_H

#include "../observability/AccessProcessMetrics.h"
#include "AccessControlPlaneSupervisor.h"
#include "AccessDataPlaneService.h"
#include "AccessServerConfig.h"

#include <expected>
#include <memory>

#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/TcpListener.h>

namespace fiber::access_server {

struct AccessRuntimeComponents {
    std::unique_ptr<AccessControlPlaneSupervisor> control_plane;
    std::unique_ptr<AccessDataPlaneService> data_plane;
};

class AccessRuntimeFactory final {
public:
    [[nodiscard]] static std::expected<AccessRuntimeComponents, AccessServerRuntimeError>
    create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &compiler_loop,
           event::EventLoop &cat_loop, event::EventLoopGroup &http_workers, const AccessServerConfig &config,
           const net::ListenOptions &listen_options, AccessProcessMetricsSources process_metrics);
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_RUNTIME_FACTORY_H
