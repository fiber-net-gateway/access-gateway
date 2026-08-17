#include "AccessControlPlaneSupervisor.h"

#include <chrono>
#include <string>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Timeout.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

#ifndef FIBER_ACCESS_SERVER_BUILD_VERSION
#define FIBER_ACCESS_SERVER_BUILD_VERSION "unknown"
#endif

#ifndef FIBER_ACCESS_SERVER_BUILD_REVISION
#define FIBER_ACCESS_SERVER_BUILD_REVISION "unknown"
#endif

AccessActivationEvidenceIdentity activation_identity(const AccessActivationEndpointOptions &options) {
    return AccessActivationEvidenceIdentity{
            .instance_id = options.instance_id,
            .build_version = FIBER_ACCESS_SERVER_BUILD_VERSION,
            .build_revision = FIBER_ACCESS_SERVER_BUILD_REVISION,
            .started_at_unix_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count(),
    };
}

common::IoResult<void> start_cat_client(void *context) noexcept {
    return static_cast<cat::CatClient *>(context)->start();
}

async::Task<void> shutdown_cat_client(void *context) noexcept {
    return static_cast<cat::CatClient *>(context)->shutdown();
}

common::IoResult<void> start_nacos_client(void *context) noexcept {
    return static_cast<nacos::NacosClient *>(context)->start();
}

async::Task<void> shutdown_nacos_client(void *context) noexcept {
    return static_cast<nacos::NacosClient *>(context)->shutdown();
}

} // namespace

AccessControlPlaneSupervisor::AccessControlPlaneSupervisor(event::EventLoop &coordinator_loop,
                                                           event::EventLoop &nacos_loop,
                                                           event::EventLoop &compiler_loop, event::EventLoop &cat_loop,
                                                           event::EventLoopGroup &http_workers,
                                                           AccessControlPlaneOptions options,
                                                           AccessControlPlaneDependencies dependencies) noexcept :
    coordinator_loop_(&coordinator_loop), nacos_loop_(&nacos_loop), cat_loop_(&cat_loop),
    initial_config_timeout_(options.initial_config_timeout), cat_client_(std::move(dependencies.cat_client)),
    nacos_client_(std::move(dependencies.nacos_client)), config_service_(std::move(dependencies.config_service)),
    naming_service_(std::move(dependencies.naming_service)), cat_lifecycle_(dependencies.cat_lifecycle),
    nacos_lifecycle_(dependencies.nacos_lifecycle), config_compiler_(compiler_loop),
    runtime_metrics_(nacos_loop, options.process_metrics),
    activation_evidence_(nacos_loop, activation_identity(options.activation_endpoint)), gray_store_(http_workers),
    service_discovery_(nacos_loop, *naming_service_,
                       AccessServiceOps{.swrr_options = options.service_discovery.swrr_options,
                                        .zone = options.service_discovery.zone,
                                        .metrics_observer = runtime_metrics_.discovery().observer()}),
    route_store_(http_workers, {}, service_discovery_, std::move(options.service_discovery),
                 runtime_metrics_.discovery().observer()),
    config_watcher_(nacos_loop, config_compiler_, *config_service_, route_store_, std::move(options.config_watcher), {},
                    runtime_metrics_.config().observer(),
                    options.activation_endpoint.enabled ? activation_evidence_.route_observer()
                                                        : AccessRouteActivationEvidenceObserver{}),
    gray_watcher_(nacos_loop, *config_service_, gray_store_, std::move(options.gray_watcher),
                  options.activation_endpoint.enabled ? activation_evidence_.gray_observer()
                                                      : AccessGrayActivationEvidenceObserver{}),
    tls_certificate_store_(nacos_loop, http_workers, options.quic_enabled, runtime_metrics_.tls().observer()),
    tls_certificate_watcher_(nacos_loop, config_compiler_, *config_service_, tls_certificate_store_,
                             std::move(options.tls_certificate_watcher),
                             options.activation_endpoint.enabled ? activation_evidence_.tls_observer()
                                                                 : AccessTlsActivationEvidenceObserver{}),
    tls_enabled_(options.tls_enabled) {
    FIBER_ASSERT(config_service_);
    FIBER_ASSERT(naming_service_);
    FIBER_ASSERT((cat_lifecycle_.start == nullptr) == (cat_lifecycle_.shutdown == nullptr));
    FIBER_ASSERT((nacos_lifecycle_.start == nullptr) == (nacos_lifecycle_.shutdown == nullptr));
    if (!cat_lifecycle_ && cat_client_) {
        cat_lifecycle_ = AccessControlResourceLifecycle{
                .context = cat_client_.get(),
                .start = &start_cat_client,
                .shutdown = &shutdown_cat_client,
        };
    }
    if (!nacos_lifecycle_) {
        FIBER_ASSERT(nacos_client_);
        nacos_lifecycle_ = AccessControlResourceLifecycle{
                .context = nacos_client_.get(),
                .start = &start_nacos_client,
                .shutdown = &shutdown_nacos_client,
        };
    }
    FIBER_ASSERT(coordinator_loop_ != nacos_loop_);
    FIBER_ASSERT(coordinator_loop_ != &compiler_loop);
    FIBER_ASSERT(coordinator_loop_ != cat_loop_);
    FIBER_ASSERT(nacos_loop_ != &compiler_loop);
    FIBER_ASSERT(nacos_loop_ != cat_loop_);
    FIBER_ASSERT(&compiler_loop != cat_loop_);
    for (std::size_t i = 0; i < http_workers.size(); ++i) {
        FIBER_ASSERT(&http_workers.at(i) != nacos_loop_);
        FIBER_ASSERT(&http_workers.at(i) != &compiler_loop);
        FIBER_ASSERT(&http_workers.at(i) != coordinator_loop_);
        FIBER_ASSERT(&http_workers.at(i) != cat_loop_);
    }
    nacos_start_publisher_ = nacos_start_status_.acquire_publisher();
    FIBER_ASSERT(nacos_start_publisher_.has_value());
    nacos_stopped_publisher_ = nacos_stopped_.acquire_publisher();
    FIBER_ASSERT(nacos_stopped_publisher_.has_value());
    cat_start_publisher_ = cat_start_status_.acquire_publisher();
    FIBER_ASSERT(cat_start_publisher_.has_value());
    cat_stopped_publisher_ = cat_stopped_.acquire_publisher();
    FIBER_ASSERT(cat_stopped_publisher_.has_value());
}

AccessControlPlaneSupervisor::~AccessControlPlaneSupervisor() noexcept {
    FIBER_ASSERT(state_ == State::Created || state_ == State::Stopped);
    FIBER_ASSERT(nacos_start_tasks_.empty());
    FIBER_ASSERT(cat_start_tasks_.empty());
}

AccessControlPlaneLifecycle AccessControlPlaneSupervisor::lifecycle() noexcept {
    return AccessControlPlaneLifecycle{
            .context = this,
            .start = &start_lifecycle,
            .shutdown = &shutdown_lifecycle,
    };
}

async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>>
AccessControlPlaneSupervisor::start_lifecycle(void *context) noexcept {
    return static_cast<AccessControlPlaneSupervisor *>(context)->start();
}

async::Task<void> AccessControlPlaneSupervisor::shutdown_lifecycle(void *context) noexcept {
    return static_cast<AccessControlPlaneSupervisor *>(context)->shutdown();
}

async::DetachedTask AccessControlPlaneSupervisor::start_cat_on_owner() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    FIBER_ASSERT(cat_lifecycle_);
    cat_start_attempted_ = true;
    const auto started = cat_lifecycle_.start(cat_lifecycle_.context);
    if (!started) {
        cat_start_publisher_->publish(CatStartStatus{
                .error = make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::StartCatClient,
                                                             started.error()),
        });
    } else {
        cat_start_publisher_->publish(CatStartStatus{.success = true});
    }
    cat_start_tasks_.done();
    co_return;
}

async::DetachedTask AccessControlPlaneSupervisor::start_nacos_on_owner() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    FIBER_ASSERT(nacos_lifecycle_);
    const AccessDiscoveryMetricsObserver metrics = runtime_metrics_.discovery().observer();
    metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Starting);
    nacos_client_start_attempted_ = true;
    auto client_started = nacos_lifecycle_.start(nacos_lifecycle_.context);
    if (!client_started) {
        metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Failed);
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::StartNacosClient,
                                                             client_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Running);
    metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Starting);
    config_service_start_attempted_ = true;
    auto config_started = config_service_->start();
    if (!config_started) {
        metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Failed);
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::StartConfigService,
                                                             config_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Running);
    metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Starting);
    naming_service_start_attempted_ = true;
    auto naming_started = naming_service_->start();
    if (!naming_started) {
        metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Failed);
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::StartNamingService,
                                                             naming_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Running);
    auto gray_started = gray_watcher_.start();
    if (!gray_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::StartGrayWatcher,
                                                             gray_started.error().io_error,
                                                             std::move(gray_started.error().message)),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    gray_watcher_started_ = true;
    if (tls_enabled_) {
        auto tls_started = tls_certificate_watcher_.start();
        if (!tls_started) {
            nacos_start_publisher_->publish(NacosStartStatus{
                    .error = make_access_server_runtime_io_error(
                            AccessServerRuntimeErrorCode::StartTlsCertificateWatcher, tls_started.error().io_error,
                            std::move(tls_started.error().message)),
            });
            nacos_start_tasks_.done();
            co_return;
        }
        tls_certificate_watcher_started_ = true;
    }
    auto watcher_started = config_watcher_.start();
    if (!watcher_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::StartAccessWatcher,
                                                             watcher_started.error().io_error,
                                                             std::move(watcher_started.error().message)),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    config_watcher_started_ = true;
    nacos_start_publisher_->publish(NacosStartStatus{.success = true});
    nacos_start_tasks_.done();
}

async::DetachedTask AccessControlPlaneSupervisor::shutdown_nacos_on_owner() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    const AccessDiscoveryMetricsObserver metrics = runtime_metrics_.discovery().observer();
    co_await nacos_start_tasks_.join();
    if (config_watcher_started_) {
        co_await config_watcher_.shutdown();
        config_watcher_started_ = false;
    }
    if (tls_certificate_watcher_started_) {
        co_await tls_certificate_watcher_.shutdown();
        tls_certificate_watcher_started_ = false;
    }
    if (gray_watcher_started_) {
        co_await gray_watcher_.shutdown();
        gray_watcher_started_ = false;
    }
    co_await tls_certificate_store_.shutdown();
    route_store_.clear();
    co_await service_discovery_.shutdown();
    if (naming_service_start_attempted_) {
        metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Stopping);
        co_await naming_service_->shutdown();
        metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Stopped);
        naming_service_start_attempted_ = false;
    }
    if (config_service_start_attempted_) {
        metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Stopping);
        co_await config_service_->shutdown();
        metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Stopped);
        config_service_start_attempted_ = false;
    }
    if (nacos_client_start_attempted_) {
        metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Stopping);
        co_await nacos_lifecycle_.shutdown(nacos_lifecycle_.context);
        metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Stopped);
        nacos_client_start_attempted_ = false;
    }
    nacos_stopped_publisher_->publish(true);
}

async::DetachedTask AccessControlPlaneSupervisor::shutdown_cat_on_owner() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    co_await cat_start_tasks_.join();
    if (cat_start_attempted_) {
        co_await cat_lifecycle_.shutdown(cat_lifecycle_.context);
        cat_start_attempted_ = false;
    }
    cat_stopped_publisher_->publish(true);
}

async::Task<void> AccessControlPlaneSupervisor::stop_nacos() noexcept {
    FIBER_ASSERT(coordinator_loop_->in_loop());
    auto stopped = nacos_stopped_.subscribe();
    auto snapshot = stopped.current();
    if (!nacos_shutdown_spawned_) {
        nacos_shutdown_spawned_ = true;
        async::spawn(*nacos_loop_, [this]() { return shutdown_nacos_on_owner(); });
    }
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await stopped.next(snapshot.version);
    }
}

async::Task<void> AccessControlPlaneSupervisor::stop_cat() noexcept {
    FIBER_ASSERT(coordinator_loop_->in_loop());
    if (!cat_lifecycle_) {
        co_return;
    }
    auto stopped = cat_stopped_.subscribe();
    auto snapshot = stopped.current();
    if (!cat_shutdown_spawned_) {
        cat_shutdown_spawned_ = true;
        async::spawn(*cat_loop_, [this]() { return shutdown_cat_on_owner(); });
    }
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await stopped.next(snapshot.version);
    }
}

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessControlPlaneSupervisor::wait_for_access_config() noexcept {
    auto readiness = config_watcher_.subscribe_readiness();
    auto readiness_snapshot = readiness.current();
    const auto readiness_pending = [](const std::shared_ptr<const AccessConfigReadiness> &value) noexcept {
        return !value || value->state == AccessConfigReadinessState::WaitingForProjectList ||
               value->state == AccessConfigReadinessState::SynchronizingProjects;
    };
    const auto now = event::EventLoop::current().now();
    const auto maximum_deadline = std::chrono::steady_clock::time_point::max();
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(initial_config_timeout_);
    const auto deadline = timeout <= std::chrono::steady_clock::duration::zero() ? maximum_deadline
                          : timeout >= maximum_deadline - now                    ? maximum_deadline
                                                                                 : now + timeout;
    while (readiness_pending(readiness_snapshot.value)) {
        if (deadline == maximum_deadline) {
            readiness_snapshot = co_await readiness.next(readiness_snapshot.version);
            continue;
        }
        const auto current_time = event::EventLoop::current().now();
        const auto remaining =
                deadline > current_time ? deadline - current_time : std::chrono::steady_clock::duration::zero();
        auto next = co_await async::timeout_for(
                [&readiness, version = readiness_snapshot.version]() { return readiness.next(version); }, remaining);
        if (!next) {
            const AccessConfigReadiness *current = readiness_snapshot.value.get();
            std::string message = "initial access configuration synchronization timed out";
            if (current) {
                message.append(" (desired=");
                message.append(std::to_string(current->desired_projects));
                message.append(", subscribed=");
                message.append(std::to_string(current->subscribed_projects));
                message.append(", synchronized=");
                message.append(std::to_string(current->synchronized_projects));
                message.append(", retrying=");
                message.append(std::to_string(current->retrying_projects));
                message.append(", processing=");
                message.append(std::to_string(current->processing_projects));
                message.append(", ready_to_publish=");
                message.append(std::to_string(current->ready_to_publish_projects));
                message.append(", rejected=");
                message.append(std::to_string(current->rejected_projects));
                message.push_back(')');
            }
            co_return std::unexpected(make_access_server_runtime_io_error(
                    AccessServerRuntimeErrorCode::InitialConfigTimeout, common::IoErr::TimedOut, std::move(message)));
        }
        readiness_snapshot = std::move(*next);
    }
    if (!readiness_snapshot.value || readiness_snapshot.value->state != AccessConfigReadinessState::Ready) {
        const common::IoErr io_error =
                readiness_snapshot.value && readiness_snapshot.value->io_error != common::IoErr::None
                        ? readiness_snapshot.value->io_error
                        : common::IoErr::Canceled;
        std::string message = readiness_snapshot.value ? readiness_snapshot.value->message : std::string{};
        if (message.empty()) {
            message = "access configuration synchronization stopped before readiness";
        }
        co_return std::unexpected(make_access_server_runtime_io_error(
                AccessServerRuntimeErrorCode::InitialConfigUnavailable, io_error, std::move(message)));
    }
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>>
AccessControlPlaneSupervisor::wait_for_tls_certificate() noexcept {
    AccessControlPlaneReady ready;
    if (!tls_enabled_) {
        co_return ready;
    }

    auto tls_ready = tls_certificate_watcher_.subscribe_ready();
    auto tls_snapshot = tls_ready.current();
    if ((!tls_snapshot.value || !*tls_snapshot.value) && initial_config_timeout_ > std::chrono::milliseconds::zero()) {
        auto result = co_await async::when_any(
                [&tls_ready, version = tls_snapshot.version]() { return tls_ready.next(version); },
                [timeout = initial_config_timeout_]() { return async::sleep(timeout); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            co_return std::unexpected(make_access_server_runtime_io_error(
                    AccessServerRuntimeErrorCode::InitialTlsCertificateTimeout, common::IoErr::TimedOut,
                    "initial TLS certificate synchronization timed out"));
        }
        tls_snapshot = std::move(result).get<0>();
    } else if (!tls_snapshot.value || !*tls_snapshot.value) {
        while (!tls_snapshot.value || !*tls_snapshot.value) {
            tls_snapshot = co_await tls_ready.next(tls_snapshot.version);
        }
    }
    if (!tls_snapshot.value || !*tls_snapshot.value) {
        co_return std::unexpected(make_access_server_runtime_io_error(
                AccessServerRuntimeErrorCode::InitialTlsCertificateUnavailable, common::IoErr::Canceled,
                "TLS certificate subscription closed before synchronization"));
    }
    ready.tls_bootstrap = tls_certificate_store_.bootstrap_identity();
    FIBER_ASSERT(ready.tls_bootstrap);
    ready.tls_identity_selector = tls_certificate_store_.selector_ops();
    co_return ready;
}

async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>>
AccessControlPlaneSupervisor::start() noexcept {
    FIBER_ASSERT(coordinator_loop_->in_loop());
    FIBER_ASSERT(state_ == State::Created);
    state_ = State::Starting;

    if (cat_lifecycle_) {
        auto cat_status = cat_start_status_.subscribe();
        auto cat_snapshot = cat_status.current();
        cat_start_tasks_.add();
        async::spawn(*cat_loop_, [this]() { return start_cat_on_owner(); });
        while (!cat_snapshot.value) {
            cat_snapshot = co_await cat_status.next(cat_snapshot.version);
        }
        if (!cat_snapshot.value->success) {
            co_return std::unexpected(cat_snapshot.value->error);
        }
    }

    auto nacos_status = nacos_start_status_.subscribe();
    auto nacos_snapshot = nacos_status.current();
    nacos_start_tasks_.add();
    async::spawn(*nacos_loop_, [this]() { return start_nacos_on_owner(); });
    while (!nacos_snapshot.value) {
        nacos_snapshot = co_await nacos_status.next(nacos_snapshot.version);
    }
    if (!nacos_snapshot.value->success) {
        co_return std::unexpected(nacos_snapshot.value->error);
    }

    auto config_ready = co_await wait_for_access_config();
    if (!config_ready) {
        co_return std::unexpected(std::move(config_ready.error()));
    }
    auto tls_ready = co_await wait_for_tls_certificate();
    if (!tls_ready) {
        co_return std::unexpected(std::move(tls_ready.error()));
    }
    state_ = State::Running;
    co_return std::move(*tls_ready);
}

async::Task<void> AccessControlPlaneSupervisor::shutdown() noexcept {
    FIBER_ASSERT(coordinator_loop_->in_loop());
    if (state_ == State::Stopped) {
        co_return;
    }
    state_ = State::Stopping;
    co_await stop_nacos();
    co_await stop_cat();
    state_ = State::Stopped;
}

} // namespace fiber::access_server
