#ifndef FIBER_ACCESS_SERVER_ACCESS_DATA_PLANE_SERVICE_H
#define FIBER_ACCESS_SERVER_ACCESS_DATA_PLANE_SERVICE_H

#include "../execution/ClientMetadata.h"
#include "../execution/UpstreamTlsClientPolicy.h"
#include "../observability/AccessActivationEvidence.h"
#include "../observability/AccessLogPolicy.h"
#include "AccessRuntimeCoordinator.h"
#include "AccessScriptRuntime.h"
#include "AccessServer.h"

#include <cstddef>
#include <memory>

#include <fiber/cat/CatClient.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>

namespace fiber::access_server {

class AccessRuntimeMetrics;

struct AccessDataPlaneOptions {
    net::SocketAddress listen_address;
    net::SocketAddress metrics_listen_address;
    net::ListenOptions listen_options;
    http::HttpServerOptions http_server;
    AccessActivationEndpointOptions activation_endpoint;
    ClientMetadataResolverOptions client_metadata;
    AccessLogOptions access_log;
    AccessDnsServiceOptions dns = AccessDnsServiceOptions::local_default();
    AccessDnsResolverFactory dns_resolver_factory = AccessDnsResolverFactory::system();
    ProxyExecutorOptions executor;
    std::size_t default_max_request_body_size = 0;
    bool test_mode = false;
};

class AccessDataPlaneService final : public common::NonCopyable, public common::NonMovable {
public:
    AccessDataPlaneService(event::EventLoop &accept_loop, event::EventLoopGroup &http_workers,
                           const RouteConfigStore &route_store, ProxyClusterMatcher gray_matcher,
                           const AccessRuntimeMetrics &runtime_metrics,
                           const AccessActivationEvidenceStore &activation_evidence, cat::CatClient *cat_client,
                           AccessDataPlaneOptions options) noexcept;
    ~AccessDataPlaneService() noexcept;

    [[nodiscard]] AccessDataPlaneLifecycle lifecycle() noexcept;
    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>>
    start(AccessControlPlaneReady ready) noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] int fd() const noexcept { return server_ ? server_->fd() : -1; }
    [[nodiscard]] int metrics_fd() const noexcept { return server_ ? server_->metrics_fd() : -1; }

private:
    [[nodiscard]] static async::Task<std::expected<void, AccessServerRuntimeError>>
    start_lifecycle(void *context, AccessControlPlaneReady ready) noexcept;
    [[nodiscard]] static async::Task<void> shutdown_lifecycle(void *context) noexcept;
    [[nodiscard]] async::Task<void> rollback_start(AccessControlPlaneReady &ready) noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    event::EventLoopGroup *http_workers_ = nullptr;
    const RouteConfigStore *route_store_ = nullptr;
    const AccessRuntimeMetrics *runtime_metrics_ = nullptr;
    const AccessActivationEvidenceStore *activation_evidence_ = nullptr;
    cat::CatClient *cat_client_ = nullptr;
    ProxyClusterMatcher gray_matcher_;
    AccessDataPlaneOptions options_;
    AccessScriptRuntime script_runtime_;
    std::unique_ptr<AccessServer> server_;
    bool shutdown_complete_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_DATA_PLANE_SERVICE_H
