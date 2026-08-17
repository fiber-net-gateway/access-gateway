#include "AccessWorkerResources.h"

#include "../observability/AccessRequestTelemetry.h"

#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClient.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {

AccessWorkerResources::AccessWorkerResources(event::EventLoopGroup &workers, const RouteConfigStore &config_store,
                                             ProxyClusterMatcher cluster_matcher,
                                             AccessWorkerResourcesOptions options) :
    workers_(&workers), client_metadata_resolver_([&options]() {
        options.client_metadata.connection_secure = options.connection_secure;
        return std::move(options.client_metadata);
    }()),
    access_log_policy_(std::move(options.access_log)), dns_(options.dns_resolver_factory), pool_(workers),
    executor_(pool_, cluster_matcher, dns_.adapter(), std::move(options.executor)),
    handler_(config_store.snapshot_provider(), options.script_adapter,
             AccessRequestHandlerOptions{
                     .default_max_request_body_size = options.default_max_request_body_size,
                     .test_mode = options.test_mode,
             },
             executor_.adapter()),
    metrics_(workers, options.runtime_metrics), cat_client_(options.cat_client),
    http3_alt_svc_(std::move(options.http3_alt_svc)) {
    FIBER_ASSERT(workers.size() > 0);
}

AccessWorkerResources::~AccessWorkerResources() {
    metrics_.stop_collecting();
    FIBER_ASSERT(!initialized_);
    FIBER_ASSERT(cat_detach_tasks_.empty());
}

async::Task<common::IoResult<void>> AccessWorkerResources::initialize() noexcept {
    if (initialized_) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (!metrics_.valid()) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto access_log_initialized = access_log_policy_.initialize();
    if (!access_log_initialized) {
        co_return std::unexpected(access_log_initialized.error());
    }
    if (!co_await dns_.init(*workers_)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    if (!pool_.init()) {
        co_await dns_.shutdown();
        co_return std::unexpected(common::IoErr::NoMem);
    }
    initialized_ = true;
    co_return common::IoResult<void>{};
}

async::Task<void> AccessWorkerResources::shutdown() noexcept {
    metrics_.stop_collecting();
    co_await metrics_.wait_for_idle();
    co_await detach_cat_workers();
    co_await pool_.shutdown_async();
    if (initialized_) {
        co_await dns_.shutdown();
    }
    initialized_ = false;
}

async::Task<void> AccessWorkerResources::handle(http::HttpExchange &exchange) noexcept {
    AccessServerMetrics::Worker &worker = metrics_.worker(event::EventLoop::current().group_index());
    AccessRequestTelemetry telemetry(exchange, &worker, cat_client_, &access_log_policy_, &client_metadata_resolver_);
    if (!http3_alt_svc_.empty()) {
        (void) telemetry.response_headers().set("Alt-Svc", http3_alt_svc_);
    }
    co_await handler_.handle(exchange, telemetry);
}

async::DetachedTask AccessWorkerResources::detach_cat_worker() noexcept {
    if (cat_client_) {
        (void) co_await cat_client_->detach_current_event_loop();
    }
    cat_detach_tasks_.done();
}

async::Task<void> AccessWorkerResources::detach_cat_workers() noexcept {
    if (!cat_client_ || cat_client_->state() != cat::CatClientState::Running) {
        co_return;
    }
    cat_detach_tasks_.add(workers_->size());
    for (std::size_t i = 0; i < workers_->size(); ++i) {
        async::spawn(workers_->at(i), [this]() { return detach_cat_worker(); });
    }
    co_await cat_detach_tasks_.join();
}

} // namespace fiber::access_server
