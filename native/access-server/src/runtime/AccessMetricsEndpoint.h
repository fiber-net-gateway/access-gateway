#ifndef FIBER_ACCESS_SERVER_ACCESS_METRICS_ENDPOINT_H
#define FIBER_ACCESS_SERVER_ACCESS_METRICS_ENDPOINT_H

#include "../observability/AccessServerMetrics.h"
#include "AccessActivationEndpoint.h"

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1Server.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>

namespace fiber::access_server {

class AccessDiscoveryMetrics;

struct AccessMetricsEndpointOptions {
    const AccessActivationEvidenceStore *activation_evidence = nullptr;
    const AccessDiscoveryMetrics *discovery_metrics = nullptr;
    AccessActivationEndpointOptions activation;
};

// Owns the status listener and its /metrics versus activation-evidence routing.
class AccessMetricsEndpoint final : public common::NonCopyable, public common::NonMovable {
public:
    AccessMetricsEndpoint(event::EventLoop &accept_loop, event::EventLoopGroup &workers, AccessServerMetrics &metrics,
                          AccessMetricsEndpointOptions options = {});
    ~AccessMetricsEndpoint();

    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &address,
                                              const net::ListenOptions &options = {});
    async::DetachedTask serve();
    [[nodiscard]] async::Task<void> shutdown_and_wait() noexcept;
    [[nodiscard]] int fd() const noexcept { return server_.fd(); }

private:
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange) noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    AccessServerMetrics *metrics_ = nullptr;
    AccessActivationEndpoint activation_endpoint_;
    http::Http1Server server_;
    bool bound_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_METRICS_ENDPOINT_H
