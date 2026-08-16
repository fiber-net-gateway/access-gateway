#ifndef FIBER_ACCESS_SERVER_ACCESS_WORKER_RESOURCES_H
#define FIBER_ACCESS_SERVER_ACCESS_WORKER_RESOURCES_H

#include "../execution/AccessRequestHandler.h"
#include "../execution/ClientMetadata.h"
#include "../execution/ProxyExecutor.h"
#include "../observability/AccessLogPolicy.h"
#include "../observability/AccessServerMetrics.h"
#include "AccessDnsService.h"
#include "RouteConfigStore.h"

#include <cstddef>
#include <string>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/StealableHttp1ConnectionPoolSet.h>

namespace fiber::cat {
class CatClient;
}

namespace fiber::access_server {

class AccessRuntimeMetrics;

struct AccessWorkerResourcesOptions {
    std::size_t default_max_request_body_size = 400U << 20U;
    ClientMetadataResolverOptions client_metadata;
    bool connection_secure = false;
    AccessLogOptions access_log;
    AccessRequestScriptAdapter script_adapter;
    ProxyExecutorOptions executor;
    const AccessRuntimeMetrics *runtime_metrics = nullptr;
    cat::CatClient *cat_client = nullptr;
    bool test_mode = false;
    std::string http3_alt_svc;
};

// Owns every request-worker resource and their ordered asynchronous teardown.
// AccessServer remains the control-loop façade and never reaches into these
// individual facilities after construction.
class AccessWorkerResources final : public common::NonCopyable, public common::NonMovable {
public:
    AccessWorkerResources(event::EventLoopGroup &workers, const RouteConfigStore &config_store,
                          ProxyClusterMatcher cluster_matcher, AccessWorkerResourcesOptions options);
    ~AccessWorkerResources();

    [[nodiscard]] async::Task<common::IoResult<void>> initialize() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange) noexcept;

    [[nodiscard]] AccessServerMetrics &metrics() noexcept { return metrics_; }

private:
    [[nodiscard]] async::DetachedTask detach_cat_worker() noexcept;
    [[nodiscard]] async::Task<void> detach_cat_workers() noexcept;

    event::EventLoopGroup *workers_ = nullptr;
    ClientMetadataResolver client_metadata_resolver_;
    AccessLogPolicy access_log_policy_;
    AccessDnsService dns_;
    http::StealableHttp1ConnectionPoolSet pool_;
    ProxyExecutor executor_;
    AccessRequestHandler handler_;
    AccessServerMetrics metrics_;
    cat::CatClient *cat_client_ = nullptr;
    async::WaitGroup cat_detach_tasks_;
    bool initialized_ = false;
    std::string http3_alt_svc_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_WORKER_RESOURCES_H
