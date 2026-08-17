#include "AccessRuntimeFactory.h"

#include <new>
#include <utility>

#include <fiber/cat/CatClient.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {

std::expected<AccessRuntimeComponents, AccessServerRuntimeError>
AccessRuntimeFactory::create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop,
                             event::EventLoop &compiler_loop, event::EventLoop &cat_loop,
                             event::EventLoopGroup &http_workers, const AccessServerConfig &config,
                             const net::ListenOptions &listen_options, AccessProcessMetricsSources process_metrics) {
    auto dns_options = config.resolve_dns_options();
    if (!dns_options) {
        return std::unexpected(make_access_server_runtime_io_error(
                AccessServerRuntimeErrorCode::LoadDnsConfiguration, common::IoErr::Invalid,
                "failed to load bounded DNS resolver configuration"));
    }
    auto upstream_tls_validated = validate_upstream_tls_client_policy(config.upstream_tls_client_policy());
    if (!upstream_tls_validated) {
        return std::unexpected(make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::InitializeUpstreamTls,
                                                                   upstream_tls_validated.error(),
                                                                   "failed to initialize upstream TLS trust store"));
    }

    auto client = nacos::NacosClient::create(nacos_loop, config.nacos_config());
    if (!client) {
        return std::unexpected(make_access_server_runtime_create_error(AccessServerRuntimeErrorCode::CreateNacosClient,
                                                                       client.error()));
    }
    auto config_service = nacos::ConfigService::create(**client);
    if (!config_service) {
        return std::unexpected(make_access_server_runtime_create_error(
                AccessServerRuntimeErrorCode::CreateConfigService, config_service.error()));
    }
    auto naming_service = nacos::NamingService::create(**client);
    if (!naming_service) {
        return std::unexpected(make_access_server_runtime_create_error(
                AccessServerRuntimeErrorCode::CreateNamingService, naming_service.error()));
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
    process_metrics.cat_client = cat_client.get();

    auto control_plane = std::unique_ptr<AccessControlPlaneSupervisor>(new (std::nothrow) AccessControlPlaneSupervisor(
            accept_loop, nacos_loop, compiler_loop, cat_loop, http_workers,
            AccessControlPlaneOptions{
                    .initial_config_timeout = config.initial_config_timeout(),
                    .activation_endpoint = config.activation_endpoint_options(),
                    .config_watcher = config.watcher_options(),
                    .gray_watcher = config.gray_watcher_options(),
                    .tls_certificate_watcher = config.tls_certificate_watcher_options(),
                    .service_discovery = config.service_discovery_options(),
                    .process_metrics = process_metrics,
                    .tls_enabled = config.http_server_options().tls.enabled,
                    .quic_enabled = config.http_server_options().http3.enabled,
            },
            AccessControlPlaneDependencies{
                    .cat_client = std::move(cat_client),
                    .nacos_client = std::move(*client),
                    .config_service = std::move(*config_service),
                    .naming_service = std::move(*naming_service),
            }));
    if (!control_plane) {
        return std::unexpected(AccessServerRuntimeError{
                .code = AccessServerRuntimeErrorCode::AllocateRuntime,
                .create_error = nacos::NacosCreateErrorCode::NoMem,
        });
    }

    auto data_plane = std::unique_ptr<AccessDataPlaneService>(new (std::nothrow) AccessDataPlaneService(
            accept_loop, http_workers, control_plane->route_store(), control_plane->gray_matcher(),
            control_plane->runtime_metrics(), control_plane->activation_evidence(), control_plane->cat_client(),
            AccessDataPlaneOptions{
                    .listen_address = config.listen_address(),
                    .metrics_listen_address = config.metrics_listen_address(),
                    .listen_options = listen_options,
                    .http_server = config.http_server_options(),
                    .activation_endpoint = config.activation_endpoint_options(),
                    .client_metadata = config.client_metadata_options(),
                    .access_log = config.access_log_options(),
                    .dns = [&]() {
                        dns_options->metrics = control_plane->runtime_metrics().dns().observer();
                        return std::move(*dns_options);
                    }(),
                    .executor =
                            ProxyExecutorOptions{
                                    .connect_timeout = config.upstream_connect_timeout(),
                                    .happy_eyeballs = config.happy_eyeballs_policy(),
                                    .upstream_tls = config.upstream_tls_client_policy(),
                            },
                    .default_max_request_body_size = config.default_max_request_body_size(),
                    .test_mode = config.test_mode(),
            }));
    if (!data_plane) {
        return std::unexpected(AccessServerRuntimeError{
                .code = AccessServerRuntimeErrorCode::AllocateRuntime,
                .create_error = nacos::NacosCreateErrorCode::NoMem,
        });
    }

    return AccessRuntimeComponents{
            .control_plane = std::move(control_plane),
            .data_plane = std::move(data_plane),
    };
}

} // namespace fiber::access_server
