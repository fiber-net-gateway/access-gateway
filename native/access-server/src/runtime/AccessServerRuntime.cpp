#include "AccessServerRuntime.h"

#include <chrono>
#include <new>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
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

} // namespace

std::string_view access_server_runtime_stage_name(AccessServerRuntimeErrorCode code) noexcept {
    switch (code) {
        case AccessServerRuntimeErrorCode::CreateNacosClient:
            return "create Nacos client";
        case AccessServerRuntimeErrorCode::CreateConfigService:
            return "create Nacos config service";
        case AccessServerRuntimeErrorCode::CreateNamingService:
            return "create Nacos naming service";
        case AccessServerRuntimeErrorCode::CreateCatClient:
            return "create CAT client";
        case AccessServerRuntimeErrorCode::InitializeUpstreamTls:
            return "initialize upstream TLS trust store";
        case AccessServerRuntimeErrorCode::AllocateRuntime:
            return "allocate access-server runtime";
        case AccessServerRuntimeErrorCode::InitializeWorkers:
            return "initialize HTTP worker resources";
        case AccessServerRuntimeErrorCode::StartNacosClient:
            return "start Nacos client";
        case AccessServerRuntimeErrorCode::StartConfigService:
            return "start Nacos config service";
        case AccessServerRuntimeErrorCode::StartNamingService:
            return "start Nacos naming service";
        case AccessServerRuntimeErrorCode::StartCatClient:
            return "start CAT client";
        case AccessServerRuntimeErrorCode::StartGrayWatcher:
            return "subscribe gray configuration";
        case AccessServerRuntimeErrorCode::StartTlsCertificateWatcher:
            return "subscribe TLS certificate configuration";
        case AccessServerRuntimeErrorCode::StartAccessWatcher:
            return "subscribe access configuration";
        case AccessServerRuntimeErrorCode::InitialConfigUnavailable:
            return "synchronize initial access configuration";
        case AccessServerRuntimeErrorCode::InitialConfigTimeout:
            return "wait for initial access configuration";
        case AccessServerRuntimeErrorCode::InitialTlsCertificateUnavailable:
            return "receive initial TLS certificate snapshot";
        case AccessServerRuntimeErrorCode::InitialTlsCertificateTimeout:
            return "wait for initial TLS certificate snapshot";
        case AccessServerRuntimeErrorCode::Bind:
            return "bind gateway listener";
        case AccessServerRuntimeErrorCode::BindMetrics:
            return "bind Prometheus listener";
    }
    return "start access-server";
}

AccessServerRuntimeError AccessServerRuntime::make_create_error(AccessServerRuntimeErrorCode code,
                                                                nacos::NacosCreateError error) noexcept {
    return AccessServerRuntimeError{
            .code = code,
            .create_error = error.code,
    };
}

AccessServerRuntimeError AccessServerRuntime::make_io_error(AccessServerRuntimeErrorCode code, common::IoErr error,
                                                            std::string message) {
    return AccessServerRuntimeError{
            .code = code,
            .io_error = error,
            .message = std::move(message),
    };
}

std::expected<std::unique_ptr<AccessServerRuntime>, AccessServerRuntimeError>
AccessServerRuntime::create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop,
                            event::EventLoop &compiler_loop, event::EventLoop &cat_loop,
                            event::EventLoopGroup &http_workers, const AccessServerConfig &config,
                            const net::ListenOptions &listen_options) {
    auto upstream_tls_validated = validate_upstream_tls_client_policy(config.upstream_tls_client_policy());
    if (!upstream_tls_validated) {
        return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::InitializeUpstreamTls,
                                             upstream_tls_validated.error(),
                                             "failed to initialize upstream TLS trust store"));
    }
    auto client = nacos::NacosClient::create(nacos_loop, config.nacos_config());
    if (!client) {
        return std::unexpected(make_create_error(AccessServerRuntimeErrorCode::CreateNacosClient, client.error()));
    }
    auto config_service = nacos::ConfigService::create(**client);
    if (!config_service) {
        return std::unexpected(
                make_create_error(AccessServerRuntimeErrorCode::CreateConfigService, config_service.error()));
    }
    auto naming_service = nacos::NamingService::create(**client);
    if (!naming_service) {
        return std::unexpected(
                make_create_error(AccessServerRuntimeErrorCode::CreateNamingService, naming_service.error()));
    }
    std::unique_ptr<cat::CatClient> cat_client;
    if (config.cat_config()) {
        auto created = cat::CatClient::create(cat_loop, *config.cat_config());
        if (!created) {
            return std::unexpected(AccessServerRuntimeError{
                    .code = AccessServerRuntimeErrorCode::CreateCatClient,
                    .io_error = common::IoErr::Invalid,
                    .message = "failed to create CAT client",
            });
        }
        cat_client = std::move(*created);
    }

    auto runtime = std::unique_ptr<AccessServerRuntime>(new (std::nothrow) AccessServerRuntime(
            accept_loop, nacos_loop, compiler_loop, cat_loop, http_workers, config.listen_address(),
            config.http_server_options(), config.metrics_listen_address(), listen_options,
            config.activation_endpoint_options(), config.initial_config_timeout(),
            config.default_max_request_body_size(), config.test_mode(), config.client_metadata_options(),
            config.access_log_options(), config.upstream_tls_client_policy(), config.watcher_options(),
            config.gray_watcher_options(), config.tls_certificate_watcher_options(), config.service_discovery_options(),
            std::move(cat_client), std::move(*client), std::move(*config_service), std::move(*naming_service)));
    if (!runtime) {
        return std::unexpected(AccessServerRuntimeError{
                .code = AccessServerRuntimeErrorCode::AllocateRuntime,
                .create_error = nacos::NacosCreateErrorCode::NoMem,
        });
    }
    return runtime;
}

AccessServerRuntime::AccessServerRuntime(
        event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &compiler_loop,
        event::EventLoop &cat_loop, event::EventLoopGroup &http_workers, net::SocketAddress listen_address,
        http::HttpServerOptions http_server_options, net::SocketAddress metrics_listen_address,
        net::ListenOptions listen_options, AccessActivationEndpointOptions activation_endpoint_options,
        std::chrono::milliseconds initial_config_timeout, std::size_t default_max_request_body_size, bool test_mode,
        ClientMetadataResolverOptions client_metadata_options, AccessLogOptions access_log_options,
        UpstreamTlsClientPolicy upstream_tls_client_policy, AccessConfigWatcherOptions watcher_options,
        GrayConfigWatcherOptions gray_options, TlsCertificateWatcherOptions tls_certificate_options,
        AccessServiceDiscoveryOptions service_discovery_options, std::unique_ptr<cat::CatClient> cat_client,
        std::unique_ptr<nacos::NacosClient> nacos_client, std::unique_ptr<nacos::ConfigService> config_service,
        std::unique_ptr<nacos::NamingService> naming_service) noexcept :
    accept_loop_(&accept_loop), nacos_loop_(&nacos_loop), compiler_loop_(&compiler_loop), cat_loop_(&cat_loop),
    http_workers_(&http_workers), listen_address_(std::move(listen_address)),
    metrics_listen_address_(std::move(metrics_listen_address)), listen_options_(std::move(listen_options)),
    initial_config_timeout_(initial_config_timeout), default_max_request_body_size_(default_max_request_body_size),
    test_mode_(test_mode), client_metadata_options_(std::move(client_metadata_options)),
    access_log_options_(std::move(access_log_options)),
    upstream_tls_client_policy_(std::move(upstream_tls_client_policy)),
    activation_endpoint_options_(activation_endpoint_options), http_server_options_(std::move(http_server_options)),
    cat_client_(std::move(cat_client)), nacos_client_(std::move(nacos_client)),
    config_service_(std::move(config_service)), naming_service_(std::move(naming_service)),
    config_compiler_(compiler_loop), runtime_metrics_(nacos_loop),
    activation_evidence_(nacos_loop, activation_identity(activation_endpoint_options)), gray_store_(http_workers),
    service_discovery_(nacos_loop, *naming_service_,
                       AccessServiceOps{.swrr_options = service_discovery_options.swrr_options,
                                        .zone = service_discovery_options.zone,
                                        .metrics_observer = runtime_metrics_.discovery().observer()}),
    route_store_({}, service_discovery_, std::move(service_discovery_options), runtime_metrics_.discovery().observer()),
    config_watcher_(nacos_loop, config_compiler_, *config_service_, route_store_, std::move(watcher_options), {},
                    runtime_metrics_.config().observer(),
                    activation_endpoint_options.enabled ? activation_evidence_.route_observer()
                                                        : AccessRouteActivationEvidenceObserver{}),
    gray_watcher_(nacos_loop, *config_service_, gray_store_, std::move(gray_options),
                  activation_endpoint_options.enabled ? activation_evidence_.gray_observer()
                                                      : AccessGrayActivationEvidenceObserver{}),
    tls_certificate_store_(nacos_loop, http_workers, http_server_options_.http3.enabled,
                           runtime_metrics_.tls().observer()),
    tls_certificate_watcher_(nacos_loop, config_compiler_, *config_service_, tls_certificate_store_,
                             std::move(tls_certificate_options),
                             activation_endpoint_options.enabled ? activation_evidence_.tls_observer()
                                                                 : AccessTlsActivationEvidenceObserver{}) {
    FIBER_ASSERT(accept_loop_ != nacos_loop_);
    FIBER_ASSERT(accept_loop_ != compiler_loop_);
    FIBER_ASSERT(accept_loop_ != cat_loop_);
    FIBER_ASSERT(nacos_loop_ != compiler_loop_);
    FIBER_ASSERT(nacos_loop_ != cat_loop_);
    FIBER_ASSERT(compiler_loop_ != cat_loop_);
    for (std::size_t i = 0; i < http_workers.size(); ++i) {
        FIBER_ASSERT(&http_workers.at(i) != nacos_loop_);
        FIBER_ASSERT(&http_workers.at(i) != compiler_loop_);
        FIBER_ASSERT(&http_workers.at(i) != accept_loop_);
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

AccessServerRuntime::~AccessServerRuntime() {
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created || state_ == AccessServerRuntimeState::Stopped);
    FIBER_ASSERT(nacos_start_tasks_.empty());
    FIBER_ASSERT(cat_start_tasks_.empty());
}

async::DetachedTask AccessServerRuntime::start_cat() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    const auto started = cat_client_->start();
    if (!started) {
        cat_start_publisher_->publish(CatStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartCatClient, started.error()),
        });
    } else {
        cat_start_publisher_->publish(CatStartStatus{.success = true});
    }
    cat_start_tasks_.done();
    co_return;
}

async::DetachedTask AccessServerRuntime::start_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    const AccessDiscoveryMetricsObserver metrics = runtime_metrics_.discovery().observer();
    metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Starting);
    auto client_started = nacos_client_->start();
    if (!client_started) {
        metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Failed);
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartNacosClient, client_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Running);
    metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Starting);
    auto config_started = config_service_->start();
    if (!config_started) {
        metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Failed);
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartConfigService, config_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Running);
    metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Starting);
    auto naming_started = naming_service_->start();
    if (!naming_started) {
        metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Failed);
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartNamingService, naming_started.error()),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Running);
    auto gray_started = gray_watcher_.start();
    if (!gray_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartGrayWatcher, gray_started.error().io_error,
                                       std::move(gray_started.error().message)),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    if (http_server_options_.tls.enabled) {
        auto tls_started = tls_certificate_watcher_.start();
        if (!tls_started) {
            nacos_start_publisher_->publish(NacosStartStatus{
                    .error = make_io_error(AccessServerRuntimeErrorCode::StartTlsCertificateWatcher,
                                           tls_started.error().io_error, std::move(tls_started.error().message)),
            });
            nacos_start_tasks_.done();
            co_return;
        }
    }
    auto watcher_started = config_watcher_.start();
    if (!watcher_started) {
        nacos_start_publisher_->publish(NacosStartStatus{
                .error = make_io_error(AccessServerRuntimeErrorCode::StartAccessWatcher,
                                       watcher_started.error().io_error, std::move(watcher_started.error().message)),
        });
        nacos_start_tasks_.done();
        co_return;
    }
    nacos_start_publisher_->publish(NacosStartStatus{.success = true});
    nacos_start_tasks_.done();
}

async::DetachedTask AccessServerRuntime::shutdown_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    const AccessDiscoveryMetricsObserver metrics = runtime_metrics_.discovery().observer();
    co_await nacos_start_tasks_.join();
    co_await config_watcher_.shutdown();
    co_await gray_watcher_.shutdown();
    co_await tls_certificate_watcher_.shutdown();
    co_await tls_certificate_store_.shutdown();
    route_store_.clear();
    co_await service_discovery_.shutdown();
    metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Stopping);
    co_await naming_service_->shutdown();
    metrics.set_lifecycle(AccessNacosComponent::NamingService, AccessNacosLifecycleState::Stopped);
    metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Stopping);
    co_await config_service_->shutdown();
    metrics.set_lifecycle(AccessNacosComponent::ConfigService, AccessNacosLifecycleState::Stopped);
    metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Stopping);
    co_await nacos_client_->shutdown();
    metrics.set_lifecycle(AccessNacosComponent::Client, AccessNacosLifecycleState::Stopped);
    nacos_stopped_publisher_->publish(true);
}

async::DetachedTask AccessServerRuntime::shutdown_cat() noexcept {
    FIBER_ASSERT(cat_loop_->in_loop());
    co_await cat_start_tasks_.join();
    co_await cat_client_->shutdown();
    cat_stopped_publisher_->publish(true);
}

async::Task<void> AccessServerRuntime::stop_nacos() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    auto stopped = nacos_stopped_.subscribe();
    auto snapshot = stopped.current();
    if (!nacos_shutdown_spawned_) {
        nacos_shutdown_spawned_ = true;
        async::spawn(*nacos_loop_, [this]() { return shutdown_nacos(); });
    }
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await stopped.next(snapshot.version);
    }
}

async::Task<void> AccessServerRuntime::stop_cat() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (!cat_client_) {
        co_return;
    }
    auto stopped = cat_stopped_.subscribe();
    auto snapshot = stopped.current();
    if (!cat_shutdown_spawned_) {
        cat_shutdown_spawned_ = true;
        async::spawn(*cat_loop_, [this]() { return shutdown_cat(); });
    }
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await stopped.next(snapshot.version);
    }
}

async::Task<void> AccessServerRuntime::fail_start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    state_ = AccessServerRuntimeState::Stopping;
    if (server_) {
        co_await server_->shutdown_and_wait();
    }
    co_await stop_cat();
    co_await stop_nacos();
    state_ = AccessServerRuntimeState::Stopped;
}

async::Task<std::expected<void, AccessServerRuntimeError>> AccessServerRuntime::start() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(state_ == AccessServerRuntimeState::Created);
    state_ = AccessServerRuntimeState::Starting;

    if (cat_client_) {
        auto cat_status = cat_start_status_.subscribe();
        auto cat_snapshot = cat_status.current();
        cat_start_tasks_.add();
        async::spawn(*cat_loop_, [this]() { return start_cat(); });
        while (!cat_snapshot.value) {
            cat_snapshot = co_await cat_status.next(cat_snapshot.version);
        }
        if (!cat_snapshot.value->success) {
            AccessServerRuntimeError error = cat_snapshot.value->error;
            co_await fail_start();
            co_return std::unexpected(std::move(error));
        }
    }

    auto nacos_status = nacos_start_status_.subscribe();
    auto nacos_snapshot = nacos_status.current();
    nacos_start_tasks_.add();
    async::spawn(*nacos_loop_, [this]() { return start_nacos(); });
    while (!nacos_snapshot.value) {
        nacos_snapshot = co_await nacos_status.next(nacos_snapshot.version);
    }
    if (!nacos_snapshot.value->success) {
        AccessServerRuntimeError error = nacos_snapshot.value->error;
        co_await fail_start();
        co_return std::unexpected(std::move(error));
    }

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
        const auto now = event::EventLoop::current().now();
        const auto remaining = deadline > now ? deadline - now : std::chrono::steady_clock::duration::zero();
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
            co_await fail_start();
            co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::InitialConfigTimeout,
                                                    common::IoErr::TimedOut, std::move(message)));
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
        co_await fail_start();
        co_return std::unexpected(
                make_io_error(AccessServerRuntimeErrorCode::InitialConfigUnavailable, io_error, std::move(message)));
    }

    std::shared_ptr<TlsBootstrapIdentity> bootstrap;
    if (http_server_options_.tls.enabled) {
        auto tls_ready = tls_certificate_watcher_.subscribe_ready();
        auto tls_snapshot = tls_ready.current();
        if ((!tls_snapshot.value || !*tls_snapshot.value) &&
            initial_config_timeout_ > std::chrono::milliseconds::zero()) {
            auto result = co_await async::when_any(
                    [&tls_ready, version = tls_snapshot.version]() { return tls_ready.next(version); },
                    [timeout = initial_config_timeout_]() { return async::sleep(timeout); });
            if (result.is<1>()) {
                std::move(result).get<1>();
                co_await fail_start();
                co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::InitialTlsCertificateTimeout,
                                                        common::IoErr::TimedOut,
                                                        "initial TLS certificate synchronization timed out"));
            }
            tls_snapshot = std::move(result).get<0>();
        } else if (!tls_snapshot.value || !*tls_snapshot.value) {
            while (!tls_snapshot.value || !*tls_snapshot.value) {
                tls_snapshot = co_await tls_ready.next(tls_snapshot.version);
            }
        }
        if (!tls_snapshot.value || !*tls_snapshot.value) {
            co_await fail_start();
            co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::InitialTlsCertificateUnavailable,
                                                    common::IoErr::Canceled,
                                                    "TLS certificate subscription closed before synchronization"));
        }
        bootstrap = tls_certificate_store_.bootstrap_identity();
        FIBER_ASSERT(bootstrap);
        http_server_options_.tls.cert_file = bootstrap->certificate_path();
        http_server_options_.tls.key_file = bootstrap->private_key_path();
        http_server_options_.tls.identity_selector_ops = tls_certificate_store_.selector_ops();
    }

    server_.reset(new (std::nothrow) AccessServer(
            *accept_loop_, *http_workers_, route_store_, gray_store_.adapter(),
            AccessServerOptions{
                    .default_max_request_body_size = default_max_request_body_size_,
                    .client_metadata = client_metadata_options_,
                    .access_log = access_log_options_,
                    .script_adapter = script_runtime_.request_adapter(),
                    .executor =
                            ProxyExecutorOptions{
                                    .upstream_tls = std::move(upstream_tls_client_policy_),
                            },
                    .runtime_metrics = &runtime_metrics_,
                    .activation_evidence = &activation_evidence_,
                    .activation_endpoint = std::move(activation_endpoint_options_),
                    .cat_client = cat_client_.get(),
                    .test_mode = test_mode_,
                    .http_server = http_server_options_,
                    .http3_alt_svc = http_server_options_.http3.enabled
                                             ? "h3=\":" + std::to_string(listen_address_.port()) + "\"; ma=86400"
                                             : std::string{},
            }));
    if (!server_) {
        co_await fail_start();
        co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::InitializeWorkers, common::IoErr::NoMem,
                                                "failed to allocate access server"));
    }
    auto initialized = co_await server_->initialize();
    if (!initialized) {
        AccessServerRuntimeError error =
                make_io_error(AccessServerRuntimeErrorCode::InitializeWorkers, initialized.error());
        co_await fail_start();
        co_return std::unexpected(std::move(error));
    }

    auto bound = server_->bind(listen_address_, listen_options_);
    if (bootstrap) {
        bootstrap->close();
    }
    if (!bound) {
        const common::IoErr error = bound.error();
        co_await fail_start();
        co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::Bind, error));
    }
    auto metrics_bound = server_->bind_metrics(metrics_listen_address_, listen_options_);
    if (!metrics_bound) {
        const common::IoErr error = metrics_bound.error();
        co_await fail_start();
        co_return std::unexpected(make_io_error(AccessServerRuntimeErrorCode::BindMetrics, error));
    }
    state_ = AccessServerRuntimeState::Running;
    async::spawn([this]() { return server_->serve(); });
    async::spawn([this]() { return server_->serve_metrics(); });
    co_return std::expected<void, AccessServerRuntimeError>{};
}

async::Task<void> AccessServerRuntime::shutdown() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (state_ == AccessServerRuntimeState::Stopped) {
        co_return;
    }
    state_ = AccessServerRuntimeState::Stopping;
    if (server_) {
        co_await server_->shutdown_and_wait();
    }
    co_await stop_cat();
    co_await stop_nacos();
    state_ = AccessServerRuntimeState::Stopped;
}

} // namespace fiber::access_server
