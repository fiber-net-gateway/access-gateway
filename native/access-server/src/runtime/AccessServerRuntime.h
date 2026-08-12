#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_H

#include "AccessConfigWatcher.h"
#include "AccessScriptRuntime.h"
#include "AccessServer.h"
#include "AccessServerConfig.h"
#include "AccessServiceDiscovery.h"
#include "GrayConfigWatcher.h"
#include "GrayMatchStore.h"
#include "RouteConfigStore.h"
#include "TlsCertificateStore.h"
#include "TlsCertificateWatcher.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/cat/CatClient.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosCreateError.h>
#include <fiber/nacos/NamingService.h>
#include <fiber/net/TcpListener.h>

namespace fiber::access_server {

enum class AccessServerRuntimeErrorCode : std::uint8_t {
    CreateNacosClient,
    CreateConfigService,
    CreateNamingService,
    CreateCatClient,
    AllocateRuntime,
    InitializeWorkers,
    StartNacosClient,
    StartConfigService,
    StartNamingService,
    StartCatClient,
    StartGrayWatcher,
    StartTlsCertificateWatcher,
    StartAccessWatcher,
    InitialConfigUnavailable,
    InitialConfigTimeout,
    InitialTlsCertificateUnavailable,
    InitialTlsCertificateTimeout,
    Bind,
    BindMetrics,
};

struct AccessServerRuntimeError {
    AccessServerRuntimeErrorCode code = AccessServerRuntimeErrorCode::AllocateRuntime;
    common::IoErr io_error = common::IoErr::None;
    nacos::NacosCreateErrorCode create_error = nacos::NacosCreateErrorCode::InvalidState;
    std::string message;
};

enum class AccessServerRuntimeState : std::uint8_t {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
};

class AccessServerRuntime final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<AccessServerRuntime>, AccessServerRuntimeError>
    create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
           event::EventLoopGroup &http_workers, const AccessServerConfig &config,
           const net::ListenOptions &listen_options = {});

    ~AccessServerRuntime();

    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] AccessServerRuntimeState state() const noexcept { return state_; }
    [[nodiscard]] int fd() const noexcept { return server_ ? server_->fd() : -1; }
    [[nodiscard]] int metrics_fd() const noexcept { return server_ ? server_->metrics_fd() : -1; }

private:
    struct NacosStartStatus {
        bool success = false;
        AccessServerRuntimeError error;
    };

    struct CatStartStatus {
        bool success = false;
        AccessServerRuntimeError error;
    };

    AccessServerRuntime(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
                        event::EventLoopGroup &http_workers, net::SocketAddress listen_address,
                        http::HttpServerOptions http_server_options, net::SocketAddress metrics_listen_address,
                        net::ListenOptions listen_options, std::chrono::milliseconds initial_config_timeout,
                        std::size_t default_max_request_body_size, bool test_mode,
                        AccessConfigWatcherOptions watcher_options, GrayConfigWatcherOptions gray_options,
                        TlsCertificateWatcherOptions tls_certificate_options,
                        AccessServiceDiscoveryOptions service_discovery_options,
                        std::unique_ptr<cat::CatClient> cat_client, std::unique_ptr<nacos::NacosClient> nacos_client,
                        std::unique_ptr<nacos::ConfigService> config_service,
                        std::unique_ptr<nacos::NamingService> naming_service) noexcept;

    [[nodiscard]] static AccessServerRuntimeError make_create_error(AccessServerRuntimeErrorCode code,
                                                                    nacos::NacosCreateError error) noexcept;
    [[nodiscard]] static AccessServerRuntimeError make_io_error(AccessServerRuntimeErrorCode code, common::IoErr error,
                                                                std::string message = {});

    [[nodiscard]] async::DetachedTask start_nacos() noexcept;
    [[nodiscard]] async::DetachedTask start_cat() noexcept;
    [[nodiscard]] async::DetachedTask shutdown_nacos() noexcept;
    [[nodiscard]] async::DetachedTask shutdown_cat() noexcept;
    [[nodiscard]] async::Task<void> stop_nacos() noexcept;
    [[nodiscard]] async::Task<void> stop_cat() noexcept;
    [[nodiscard]] async::Task<void> fail_start() noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    event::EventLoop *nacos_loop_ = nullptr;
    event::EventLoop *cat_loop_ = nullptr;
    event::EventLoopGroup *http_workers_ = nullptr;
    net::SocketAddress listen_address_;
    net::SocketAddress metrics_listen_address_;
    net::ListenOptions listen_options_;
    std::chrono::milliseconds initial_config_timeout_{0};
    std::size_t default_max_request_body_size_ = 0;
    bool test_mode_ = false;
    http::HttpServerOptions http_server_options_;
    std::unique_ptr<cat::CatClient> cat_client_;
    std::unique_ptr<nacos::NacosClient> nacos_client_;
    std::unique_ptr<nacos::ConfigService> config_service_;
    std::unique_ptr<nacos::NamingService> naming_service_;
    AccessScriptRuntime script_runtime_;
    GrayMatchStore gray_store_;
    AccessServiceDiscovery service_discovery_;
    RouteConfigStore route_store_;
    AccessConfigWatcher config_watcher_;
    GrayConfigWatcher gray_watcher_;
    TlsCertificateStore tls_certificate_store_;
    TlsCertificateWatcher tls_certificate_watcher_;
    std::unique_ptr<AccessServer> server_;
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
    AccessServerRuntimeState state_ = AccessServerRuntimeState::Created;
    bool nacos_shutdown_spawned_ = false;
    bool cat_shutdown_spawned_ = false;
};

[[nodiscard]] std::string_view access_server_runtime_stage_name(AccessServerRuntimeErrorCode code) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_H
