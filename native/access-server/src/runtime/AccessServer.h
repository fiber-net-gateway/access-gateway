#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_H

#include "AccessMetricsEndpoint.h"
#include "AccessWorkerResources.h"
#include "RouteConfigStore.h"

#include <cstddef>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpServer.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>

namespace fiber::access_server {

class AccessRuntimeMetrics;

struct AccessServerOptions {
    std::size_t default_max_request_body_size = 400U << 20U;
    ClientMetadataResolverOptions client_metadata;
    AccessLogOptions access_log;
    AccessRequestScriptAdapter script_adapter;
    ProxyExecutorOptions executor;
    const AccessRuntimeMetrics *runtime_metrics = nullptr;
    const AccessActivationEvidenceStore *activation_evidence = nullptr;
    AccessActivationEndpointOptions activation_endpoint;
    cat::CatClient *cat_client = nullptr;
    bool test_mode = false;
    http::HttpServerOptions http_server;
    std::string http3_alt_svc;
};

class AccessServer final : public common::NonCopyable, public common::NonMovable {
public:
    AccessServer(event::EventLoop &accept_loop, event::EventLoopGroup &workers, const RouteConfigStore &config_store,
                 ProxyClusterMatcher cluster_matcher, AccessServerOptions options = {});
    ~AccessServer();

    [[nodiscard]] async::Task<common::IoResult<void>> initialize() noexcept;
    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &address,
                                              const net::ListenOptions &options = {});
    [[nodiscard]] common::IoResult<void> bind_metrics(const net::SocketAddress &address,
                                                      const net::ListenOptions &options = {});
    async::DetachedTask serve();
    async::DetachedTask serve_metrics();
    [[nodiscard]] async::Task<void> shutdown_and_wait() noexcept;
    [[nodiscard]] int fd() const noexcept { return server_.fd(); }
    [[nodiscard]] int metrics_fd() const noexcept { return metrics_endpoint_.fd(); }

private:
    event::EventLoop *accept_loop_ = nullptr;
    AccessWorkerResources worker_resources_;
    http::HttpServer server_;
    AccessMetricsEndpoint metrics_endpoint_;
    bool initialized_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_H
