#include "AccessRuntimeFactory.h"

#include "LocalFileConfigService.h"

#include <new>
#include <utility>

#include <fiber/cat/CatClient.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {

#ifndef FIBER_ACCESS_SERVER_BUILD_VERSION
#define FIBER_ACCESS_SERVER_BUILD_VERSION "unknown"
#endif

std::expected<AccessRuntimeComponents, AccessServerRuntimeError>
AccessRuntimeFactory::create(event::EventLoop &accept_loop, event::EventLoop &nacos_loop, event::EventLoop &cat_loop,
                             event::EventLoopGroup &http_workers, const AccessServerConfig &config,
                             const net::ListenOptions &listen_options, AccessProcessMetricsSources process_metrics) {
    // The deployment entry network is what makes X-Entry server-authoritative
    // (the value nginx used to inject), so an instance without it must fail
    // fast instead of serving entry-gated traffic with a client-supplied or
    // missing entry.
    if (config.network_entry().empty()) {
        return std::unexpected(make_access_server_runtime_io_error(
                AccessServerRuntimeErrorCode::ValidateNetworkEntry, common::IoErr::Invalid,
                "ACCESS_SERVER_NETWORK_ENTRY is required: declare the deployment entry network (internet, vdi, "
                "desktop)"));
    }
    auto dns_options = config.resolve_dns_options();
    if (!dns_options) {
        return std::unexpected(make_access_server_runtime_io_error(
                AccessServerRuntimeErrorCode::LoadDnsConfiguration, common::IoErr::Invalid,
                "failed to load bounded DNS resolver configuration"));
    }
    auto upstream_tls_policy = prepare_upstream_tls_client_policy(config.upstream_tls_client_policy());
    if (!upstream_tls_policy) {
        return std::unexpected(make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::InitializeUpstreamTls,
                                                                   upstream_tls_policy.error(),
                                                                   "failed to initialize upstream TLS trust store"));
    }

    std::unique_ptr<nacos::NacosClient> nacos_client;
    std::unique_ptr<nacos::ConfigService> config_service;
    std::unique_ptr<nacos::NamingService> naming_service;
    void *config_reload_context = nullptr;
    AccessControlPlaneDependencies::ReloadFunction config_reload = nullptr;

    if (config.config_provider() == AccessConfigProvider::File) {
        LocalFileConfigLayout layout{
                .project_list_data_id = config.watcher_options().project_list_data_id,
                .project_route_data_id_prefix = config.watcher_options().project_route_data_id_prefix,
                .project_route_group = config.watcher_options().project_route_group,
                .tls_certificates_data_id = config.tls_certificate_watcher_options().data_id,
                .tls_certificates_group = config.tls_certificate_watcher_options().group,
                .edge_functions_data_id = std::string(kEdgeFunctionsIndexDataId),
                .edge_functions_group = config.watcher_options().project_route_group,
                .edge_function_script_data_id_prefix = std::string(kEdgeFunctionDataIdPrefix),
                .gray_data_id = config.gray_watcher_options().data_id,
                .gray_group = config.gray_watcher_options().group,
                .temp_cluster_data_id = config.temp_cluster_watcher_options().data_id,
                .temp_cluster_group = config.temp_cluster_watcher_options().group,
        };
        auto file_service = std::unique_ptr<LocalFileConfigService>(new (std::nothrow) LocalFileConfigService(
                nacos_loop, LocalFileConfigServiceOptions{
                                    .directory = std::string(config.config_dir()),
                                    .layout = std::move(layout),
                                    .poll_interval = config.config_poll_interval(),
                            }));
        auto standalone_naming = std::unique_ptr<LocalStandaloneNamingService>(
                new (std::nothrow) LocalStandaloneNamingService(nacos_loop));
        if (!file_service || !standalone_naming) {
            return std::unexpected(AccessServerRuntimeError{
                    .code = AccessServerRuntimeErrorCode::AllocateRuntime,
                    .create_error = nacos::NacosCreateErrorCode::NoMem,
            });
        }
        config_reload_context = file_service.get();
        config_reload = [](void *ctx) noexcept { static_cast<LocalFileConfigService *>(ctx)->trigger_reload(); };
        config_service = std::move(file_service);
        naming_service = std::move(standalone_naming);
    } else {
        auto client = nacos::NacosClient::create(nacos_loop, config.nacos_config());
        if (!client) {
            return std::unexpected(make_access_server_runtime_create_error(
                    AccessServerRuntimeErrorCode::CreateNacosClient, client.error()));
        }
        auto created_config = nacos::ConfigService::create(**client);
        if (!created_config) {
            return std::unexpected(make_access_server_runtime_create_error(
                    AccessServerRuntimeErrorCode::CreateConfigService, created_config.error()));
        }
        auto created_naming = nacos::NamingService::create(**client);
        if (!created_naming) {
            return std::unexpected(make_access_server_runtime_create_error(
                    AccessServerRuntimeErrorCode::CreateNamingService, created_naming.error()));
        }
        nacos_client = std::move(*client);
        config_service = std::move(*created_config);
        naming_service = std::move(*created_naming);
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

    AccessInstanceRegistrationOptions instance_registration = config.instance_registration_options();
    instance_registration.metadata = {
            nacos::NamingMetadataEntry{.key = "v", .value = FIBER_ACCESS_SERVER_BUILD_VERSION},
            nacos::NamingMetadataEntry{.key = "g", .value = config.watcher_options().project_list_data_id},
    };
    auto control_plane = std::unique_ptr<AccessControlPlaneSupervisor>(new (std::nothrow) AccessControlPlaneSupervisor(
            accept_loop, nacos_loop, cat_loop, http_workers,
            AccessControlPlaneOptions{
                    .initial_config_timeout = config.initial_config_timeout(),
                    .activation_endpoint = config.activation_endpoint_options(),
                    .config_watcher = config.watcher_options(),
                    .gray_watcher = config.gray_watcher_options(),
                    .temp_cluster_watcher = config.temp_cluster_watcher_options(),
                    .tls_certificate_watcher = config.tls_certificate_watcher_options(),
                    .service_discovery = config.service_discovery_options(),
                    .instance_registration = std::move(instance_registration),
                    .process_metrics = process_metrics,
                    .tls_enabled = config.tls_http_server_options().tls.enabled(),
                    .quic_enabled = config.tls_http_server_options().http3_enabled,
                    .test_mode = config.test_mode(),
            },
            AccessControlPlaneDependencies{
                    .cat_client = std::move(cat_client),
                    .nacos_client = std::move(nacos_client),
                    .config_service = std::move(config_service),
                    .naming_service = std::move(naming_service),
                    .config_reload_context = config_reload_context,
                    .config_reload = config_reload,
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
                    .listen_address = config.tls_listen_address(),
                    .metrics_listen_address = config.metrics_listen_address(),
                    .listen_options = listen_options,
                    .http_server = config.tls_http_server_options(),
                    .plain_listen_enabled = config.plain_listen_enabled(),
                    .plain_listen_address = config.plain_listen_address(),
                    .plain_http_server = config.plain_http_server_options(),
                    .registration_listener = config.registration_listener(),
                    .activation_endpoint = config.activation_endpoint_options(),
                    .client_metadata = config.client_metadata_options(),
                    .network_entry = std::string(config.network_entry()),
                    .dns =
                            [&]() {
                                dns_options->metrics = control_plane->runtime_metrics().dns().observer();
                                return std::move(*dns_options);
                            }(),
                    .executor =
                            ProxyExecutorOptions{
                                    .connect_timeout = config.upstream_connect_timeout(),
                                    .happy_eyeballs = config.happy_eyeballs_policy(),
                                    .upstream_tls = std::move(*upstream_tls_policy),
                                    .default_max_response_body_size = config.default_max_response_body_size(),
                                    .cluster_fallback = control_plane->cluster_fallback(),
                            },
                    .default_max_request_body_size = config.default_max_request_body_size(),
                    .test_mode = config.test_mode(),
                    .edge_function_fail_close = config.edge_function_fail_close(),
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
