#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_H

#include "AccessHttpListenerOptions.h"
#include "AccessMetricsEndpoint.h"
#include "AccessWorkerResources.h"
#include "RouteConfigStore.h"

#include <cstddef>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Server.h>
#include <fiber/http/endpoint/Http1Endpoint.h>
#include <fiber/http/endpoint/Http2Endpoint.h>
#include <fiber/http/endpoint/Http3Endpoint.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>

namespace fiber::access_server {

class AccessRuntimeMetrics;

struct AccessServerOptions {
    std::size_t default_max_request_body_size = 400U << 20U;
    ClientMetadataResolverOptions client_metadata;
    std::string network_entry;
    AccessLogOptions access_log;
    AccessDnsServiceOptions dns = AccessDnsServiceOptions::local_default();
    AccessDnsResolverFactory dns_resolver_factory = AccessDnsResolverFactory::system();
    AccessRequestScriptAdapter script_adapter;
    ProxyExecutorOptions executor;
    const AccessRuntimeMetrics *runtime_metrics = nullptr;
    const AccessActivationEvidenceStore *activation_evidence = nullptr;
    AccessActivationEndpointOptions activation_endpoint;
    cat::CatClient *cat_client = nullptr;
    bool test_mode = false;
    AccessHttpListenerOptions http_server;
    std::string http3_alt_svc;
    bool plain_listen_enabled = false;
    AccessHttpListenerOptions plain_http_server;
};

class AccessServer final : public common::NonCopyable, public common::NonMovable {
public:
    AccessServer(event::EventLoop &accept_loop, event::EventLoopGroup &workers, const RouteConfigStore &config_store,
                 ProxyClusterMatcher cluster_matcher, AccessServerOptions options = {});
    ~AccessServer();

    [[nodiscard]] async::Task<common::IoResult<void>> initialize() noexcept;
    // Stages the listener endpoints and binds them. Each bind may run at most
    // once, after initialize() and before serve(); the listeners it starts are
    // torn down by shutdown_and_wait().
    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &address,
                                              const net::ListenOptions &options = {});
    [[nodiscard]] common::IoResult<void> bind_plain(const net::SocketAddress &address,
                                                    const net::ListenOptions &options = {});
    [[nodiscard]] common::IoResult<void> bind_metrics(const net::SocketAddress &address,
                                                      const net::ListenOptions &options = {});
    async::DetachedTask serve();
    async::DetachedTask serve_metrics();
    [[nodiscard]] async::Task<void> shutdown_and_wait() noexcept;
    [[nodiscard]] int fd() const noexcept { return tls_endpoint_ ? tls_endpoint_->listener_fd() : -1; }
    [[nodiscard]] int plain_fd() const noexcept { return plain_endpoint_ ? plain_endpoint_->listener_fd() : -1; }
    [[nodiscard]] int metrics_fd() const noexcept { return metrics_endpoint_.fd(); }

private:
    event::EventLoop *accept_loop_ = nullptr;
    AccessWorkerResources worker_resources_;
    AccessHttpListenerOptions http_options_;
    AccessHttpListenerOptions plain_options_;
    http::Server server_;
    http::Server plain_server_;
    AccessMetricsEndpoint metrics_endpoint_;
    http::Http2Endpoint *tls_endpoint_ = nullptr;
    http::Http3Endpoint *http3_endpoint_ = nullptr;
    http::Http1Endpoint *plain_endpoint_ = nullptr;
    // One count per serve task spawned by serve()/serve_metrics(); drained by
    // shutdown_and_wait() so no task touches a server past its lifetime.
    async::WaitGroup serve_tasks_{};
    bool initialized_ = false;
    bool main_bound_ = false;
    bool plain_bound_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_H
