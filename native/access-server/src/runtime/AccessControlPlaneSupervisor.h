#ifndef FIBER_ACCESS_SERVER_ACCESS_CONTROL_PLANE_SUPERVISOR_H
#define FIBER_ACCESS_SERVER_ACCESS_CONTROL_PLANE_SUPERVISOR_H

#include "../observability/AccessActivationEvidence.h"
#include "../observability/AccessRuntimeMetrics.h"
#include "AccessActivationEndpoint.h"
#include "AccessConfigCompiler.h"
#include "AccessConfigWatcher.h"
#include "AccessRuntimeCoordinator.h"
#include "AccessServiceDiscovery.h"
#include "GrayConfigWatcher.h"
#include "GrayMatchStore.h"
#include "RouteConfigStore.h"
#include "TlsCertificateStore.h"
#include "TlsCertificateWatcher.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/cat/CatClient.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {

struct AccessControlPlaneDependencies {
    std::unique_ptr<cat::CatClient> cat_client;
    std::unique_ptr<nacos::NacosClient> nacos_client;
    std::unique_ptr<nacos::ConfigService> config_service;
    std::unique_ptr<nacos::NamingService> naming_service;
};

struct AccessControlPlaneOptions {
    std::chrono::milliseconds initial_config_timeout{0};
    AccessActivationEndpointOptions activation_endpoint;
    AccessConfigWatcherOptions config_watcher;
    GrayConfigWatcherOptions gray_watcher;
    TlsCertificateWatcherOptions tls_certificate_watcher;
    AccessServiceDiscoveryOptions service_discovery;
    AccessProcessMetricsSources process_metrics;
    bool tls_enabled = false;
    bool quic_enabled = false;
};

class AccessControlPlaneSupervisor final : public common::NonCopyable, public common::NonMovable {
public:
    AccessControlPlaneSupervisor(event::EventLoop &coordinator_loop, event::EventLoop &nacos_loop,
                                 event::EventLoop &compiler_loop, event::EventLoop &cat_loop,
                                 event::EventLoopGroup &http_workers, AccessControlPlaneOptions options,
                                 AccessControlPlaneDependencies dependencies) noexcept;
    ~AccessControlPlaneSupervisor() noexcept;

    [[nodiscard]] AccessControlPlaneLifecycle lifecycle() noexcept;
    [[nodiscard]] async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] RouteConfigStore &route_store() noexcept { return route_store_; }
    [[nodiscard]] ProxyClusterMatcher gray_matcher() noexcept { return gray_store_.adapter(); }
    [[nodiscard]] AccessRuntimeMetrics &runtime_metrics() noexcept { return runtime_metrics_; }
    [[nodiscard]] AccessActivationEvidenceStore &activation_evidence() noexcept { return activation_evidence_; }
    [[nodiscard]] cat::CatClient *cat_client() const noexcept { return cat_client_.get(); }

private:
    enum class State : std::uint8_t {
        Created,
        Starting,
        Running,
        Stopping,
        Stopped,
    };

    struct NacosStartStatus {
        bool success = false;
        AccessServerRuntimeError error;
    };

    struct CatStartStatus {
        bool success = false;
        AccessServerRuntimeError error;
    };

    [[nodiscard]] static async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>>
    start_lifecycle(void *context) noexcept;
    [[nodiscard]] static async::Task<void> shutdown_lifecycle(void *context) noexcept;

    [[nodiscard]] async::DetachedTask start_nacos_on_owner() noexcept;
    [[nodiscard]] async::DetachedTask start_cat_on_owner() noexcept;
    [[nodiscard]] async::DetachedTask shutdown_nacos_on_owner() noexcept;
    [[nodiscard]] async::DetachedTask shutdown_cat_on_owner() noexcept;
    [[nodiscard]] async::Task<void> stop_nacos() noexcept;
    [[nodiscard]] async::Task<void> stop_cat() noexcept;
    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> wait_for_access_config() noexcept;
    [[nodiscard]] async::Task<std::expected<AccessControlPlaneReady, AccessServerRuntimeError>>
    wait_for_tls_certificate() noexcept;

    event::EventLoop *coordinator_loop_ = nullptr;
    event::EventLoop *nacos_loop_ = nullptr;
    event::EventLoop *cat_loop_ = nullptr;
    std::chrono::milliseconds initial_config_timeout_{0};
    std::unique_ptr<cat::CatClient> cat_client_;
    std::unique_ptr<nacos::NacosClient> nacos_client_;
    std::unique_ptr<nacos::ConfigService> config_service_;
    std::unique_ptr<nacos::NamingService> naming_service_;
    AccessConfigCompiler config_compiler_;
    AccessRuntimeMetrics runtime_metrics_;
    AccessActivationEvidenceStore activation_evidence_;
    GrayMatchStore gray_store_;
    AccessServiceDiscovery service_discovery_;
    RouteConfigStore route_store_;
    AccessConfigWatcher config_watcher_;
    GrayConfigWatcher gray_watcher_;
    TlsCertificateStore tls_certificate_store_;
    TlsCertificateWatcher tls_certificate_watcher_;
    async::WaitGroup nacos_start_tasks_;
    async::WaitGroup cat_start_tasks_;
    async::Watch<NacosStartStatus> nacos_start_status_;
    std::optional<async::Watch<NacosStartStatus>::Publisher> nacos_start_publisher_;
    async::Watch<CatStartStatus> cat_start_status_;
    std::optional<async::Watch<CatStartStatus>::Publisher> cat_start_publisher_;
    async::Watch<bool> nacos_stopped_{false};
    std::optional<async::Watch<bool>::Publisher> nacos_stopped_publisher_;
    async::Watch<bool> cat_stopped_{false};
    std::optional<async::Watch<bool>::Publisher> cat_stopped_publisher_;
    State state_ = State::Created;
    bool tls_enabled_ = false;
    bool nacos_shutdown_spawned_ = false;
    bool cat_shutdown_spawned_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONTROL_PLANE_SUPERVISOR_H
