#include "../src/runtime/AccessServerConfig.h"
#include "../src/runtime/AccessServerRuntime.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {
namespace {

TEST(AccessServerConfigTest, LoadsJavaServerDefaultsAndNacosSettings) {
    auto config = AccessServerConfig::load_from_string(R"(
        # TLS identity and route configuration are both received from Nacos.
        NACOS_SERVER_ADDRESSES=127.0.0.1,127.0.0.2
    )");

    ASSERT_TRUE(config) << config.error().detail;
    EXPECT_EQ(config->listen_address().to_string(), "0.0.0.0:16688");
    EXPECT_EQ(config->metrics_listen_address().to_string(), "0.0.0.0:16689");
    EXPECT_FALSE(config->activation_endpoint_options().enabled);
    EXPECT_TRUE(config->activation_endpoint_options().instance_id.empty());
    EXPECT_TRUE(config->activation_endpoint_options().bearer_token.empty());
    EXPECT_EQ(config->initial_config_timeout(), std::chrono::seconds(60));
    EXPECT_EQ(config->default_max_request_body_size(), 400U << 20U);
    EXPECT_FALSE(config->test_mode());
    EXPECT_EQ(config->client_metadata_options().mode, ClientMetadataMode::Direct);
    EXPECT_TRUE(config->client_metadata_options().trusted_proxy_cidrs.empty());
    EXPECT_TRUE(config->client_metadata_options().connection_secure);
    EXPECT_TRUE(config->access_log_options().query_allowlist.empty());
    EXPECT_TRUE(config->access_log_options().additional_sensitive_query_keys.empty());
    EXPECT_FALSE(config->access_log_options().query_hash_enabled);
    EXPECT_EQ(config->access_log_options().success_sample_rate_bps, kAccessLogSampleScale);
    EXPECT_EQ(config->access_log_options().max_path_bytes, 2048u);
    EXPECT_EQ(config->access_log_options().max_query_bytes, 2048u);
    EXPECT_EQ(config->upstream_tls_client_policy().verification, UpstreamTlsVerificationMode::LegacyInsecure);
    EXPECT_TRUE(config->upstream_tls_client_policy().ca_file.empty());
    EXPECT_TRUE(config->http_server_options().tls.enabled);
    EXPECT_TRUE(config->http_server_options().http3.enabled);
    EXPECT_TRUE(config->http_server_options().tls.cert_file.empty());
    EXPECT_TRUE(config->http_server_options().tls.key_file.empty());
    ASSERT_EQ(config->nacos_config().server_hosts().size(), 2U);
    EXPECT_TRUE(config->nacos_config().server_hosts()[0].is_ip_literal());
    EXPECT_EQ(config->nacos_config().server_hosts()[0].value(), "127.0.0.1");
    EXPECT_TRUE(config->nacos_config().server_hosts()[1].is_ip_literal());
    EXPECT_EQ(config->nacos_config().server_hosts()[1].value(), "127.0.0.2");
    EXPECT_EQ(config->nacos_config().http_port(), 8848);
    EXPECT_EQ(config->nacos_config().grpc_port(), 9848);
    EXPECT_EQ(config->nacos_config().namespace_id(), "public");
    EXPECT_EQ(config->watcher_options().project_list_data_id, kProjectListDataId);
    EXPECT_EQ(config->watcher_options().project_route_data_id_prefix, kProjectRouteDataIdPrefix);
    EXPECT_EQ(config->watcher_options().project_route_group, kProjectRouteGroup);
    EXPECT_EQ(config->gray_watcher_options().data_id, kGrayConfigDataId);
    EXPECT_EQ(config->service_discovery_options().group, kDefaultNacosGroup);
}

TEST(AccessServerConfigTest, LoadsExplicitRuntimeAndCompatibilityKeys) {
    auto config = AccessServerConfig::load_from_string(R"(
        ACCESS_SERVER_LISTEN_ADDRESS=127.0.0.1
        ACCESS_SERVER_LISTEN_PORT=18080
        ACCESS_SERVER_TLS_ENABLED=false
        ACCESS_SERVER_HTTP3_ENABLED=false
        ACCESS_SERVER_METRICS_LISTEN_ADDRESS=127.0.0.2
        ACCESS_SERVER_METRICS_LISTEN_PORT=19090
        ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED=true
        ACCESS_SERVER_INSTANCE_ID=access-0
        ACCESS_SERVER_ACTIVATION_TOKEN=0123456789abcdef0123456789abcdef
        ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS=2500
        ACCESS_SERVER_MAX_REQUEST_BODY_SIZE=12345
        ACCESS_SERVER_TEST_MODE=true
        ACCESS_SERVER_CLIENT_METADATA_MODE=trusted_proxy
        ACCESS_SERVER_TRUSTED_PROXY_CIDRS=10.0.0.0/8,2001:db8::/32
        ACCESS_SERVER_ACCESS_LOG_QUERY_ALLOWLIST=page,requestId
        ACCESS_SERVER_ACCESS_LOG_SENSITIVE_QUERY_KEYS=otp,CustomSecret
        ACCESS_SERVER_ACCESS_LOG_QUERY_HASH_ENABLED=true
        ACCESS_SERVER_ACCESS_LOG_SUCCESS_SAMPLE_RATE_BPS=2500
        ACCESS_SERVER_ACCESS_LOG_MAX_PATH_BYTES=4096
        ACCESS_SERVER_ACCESS_LOG_MAX_QUERY_BYTES=512
        ACCESS_SERVER_PROJECTS_DATA_ID=custom.projects
        ACCESS_SERVER_ROUTE_DATA_ID_PREFIX=custom.route.
        ACCESS_SERVER_ROUTE_GROUP=CUSTOM-ROUTE
        ACCESS_SERVER_GRAY_DATA_ID=custom.gray
        ACCESS_SERVER_NAMING_GROUP=CUSTOM-GROUP
        ACCESS_SERVER_ZONE=sh
        NACOS_SERVER_ADDRESSES=10.0.0.1
        NACOS_HTTP_PORT=18848
        NACOS_GRPC_PORT=19848
        NACOS_NAMESPACE=ns
        NACOS_TENANT=tenant
        NACOS_USERNAME=user
        NACOS_PASSWORD=pass
        NACOS_CLIENT_VERSION=access-test
    )");

    ASSERT_TRUE(config) << config.error().detail;
    EXPECT_EQ(config->listen_address().to_string(), "127.0.0.1:18080");
    EXPECT_EQ(config->metrics_listen_address().to_string(), "127.0.0.2:19090");
    EXPECT_TRUE(config->activation_endpoint_options().enabled);
    EXPECT_EQ(config->activation_endpoint_options().instance_id, "access-0");
    EXPECT_EQ(config->activation_endpoint_options().bearer_token, "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(config->initial_config_timeout(), std::chrono::milliseconds(2500));
    EXPECT_EQ(config->default_max_request_body_size(), 12345U);
    EXPECT_TRUE(config->test_mode());
    EXPECT_EQ(config->client_metadata_options().mode, ClientMetadataMode::TrustedProxy);
    EXPECT_EQ(config->client_metadata_options().trusted_proxy_cidrs.size(), 2u);
    EXPECT_FALSE(config->client_metadata_options().connection_secure);
    EXPECT_EQ(config->access_log_options().query_allowlist, (std::vector<std::string>{"page", "requestId"}));
    EXPECT_EQ(config->access_log_options().additional_sensitive_query_keys,
              (std::vector<std::string>{"otp", "customsecret"}));
    EXPECT_TRUE(config->access_log_options().query_hash_enabled);
    EXPECT_EQ(config->access_log_options().success_sample_rate_bps, 2500u);
    EXPECT_EQ(config->access_log_options().max_path_bytes, 4096u);
    EXPECT_EQ(config->access_log_options().max_query_bytes, 512u);
    EXPECT_FALSE(config->http_server_options().tls.enabled);
    EXPECT_FALSE(config->http_server_options().http3.enabled);
    EXPECT_EQ(config->watcher_options().project_list_data_id, "custom.projects");
    EXPECT_EQ(config->watcher_options().project_route_data_id_prefix, "custom.route.");
    EXPECT_EQ(config->watcher_options().project_route_group, "CUSTOM-ROUTE");
    EXPECT_EQ(config->gray_watcher_options().data_id, "custom.gray");
    EXPECT_EQ(config->gray_watcher_options().group, "CUSTOM-GROUP");
    EXPECT_EQ(config->service_discovery_options().group, "CUSTOM-GROUP");
    EXPECT_EQ(config->service_discovery_options().zone, "sh");
    EXPECT_EQ(config->nacos_config().username(), "user");
    EXPECT_EQ(config->nacos_config().password(), "pass");
}

TEST(AccessServerConfigTest, RejectsIncompleteOrUnexpectedActivationCredentials) {
    const auto expect_invalid = [](std::string_view settings) {
        std::string input = "NACOS_SERVER_ADDRESSES=127.0.0.1\n";
        input.append(settings);
        auto config = AccessServerConfig::load_from_string(input);
        EXPECT_FALSE(config);
        if (!config) {
            EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
        }
    };

    expect_invalid("ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED=true\n"
                   "ACCESS_SERVER_INSTANCE_ID=access-0\n");
    expect_invalid("ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED=true\n"
                   "ACCESS_SERVER_INSTANCE_ID=bad instance\n"
                   "ACCESS_SERVER_ACTIVATION_TOKEN=0123456789abcdef0123456789abcdef\n");
    expect_invalid("ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED=true\n"
                   "ACCESS_SERVER_INSTANCE_ID=access-0\n"
                   "ACCESS_SERVER_ACTIVATION_TOKEN=short\n");
    expect_invalid("ACCESS_SERVER_ACTIVATION_TOKEN=0123456789abcdef0123456789abcdef\n");
    expect_invalid("ACCESS_SERVER_INSTANCE_ID=access-0\n");
}

TEST(AccessServerConfigTest, LoadsUpstreamTlsVerificationModes) {
    auto system_ca = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                          "ACCESS_SERVER_UPSTREAM_TLS_MODE=system_ca\n");
    ASSERT_TRUE(system_ca) << system_ca.error().detail;
    EXPECT_EQ(system_ca->upstream_tls_client_policy().verification, UpstreamTlsVerificationMode::SystemCa);
    EXPECT_TRUE(system_ca->upstream_tls_client_policy().ca_file.empty());

    auto custom_ca =
            AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                 "ACCESS_SERVER_UPSTREAM_TLS_MODE=custom_ca\n"
                                                 "ACCESS_SERVER_UPSTREAM_TLS_CA_FILE=/run/secrets/upstream-ca.pem\n");
    ASSERT_TRUE(custom_ca) << custom_ca.error().detail;
    EXPECT_EQ(custom_ca->upstream_tls_client_policy().verification, UpstreamTlsVerificationMode::CustomCa);
    EXPECT_EQ(custom_ca->upstream_tls_client_policy().ca_file, "/run/secrets/upstream-ca.pem");
}

TEST(AccessServerConfigTest, RejectsInvalidUpstreamTlsVerificationSettings) {
    const auto expect_invalid = [](std::string_view settings) {
        std::string input = "NACOS_SERVER_ADDRESSES=127.0.0.1\n";
        input.append(settings);
        auto config = AccessServerConfig::load_from_string(input);
        EXPECT_FALSE(config);
        if (!config) {
            EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
        }
    };

    expect_invalid("ACCESS_SERVER_UPSTREAM_TLS_MODE=verify\n");
    expect_invalid("ACCESS_SERVER_UPSTREAM_TLS_MODE=custom_ca\n");
    expect_invalid("ACCESS_SERVER_UPSTREAM_TLS_CA_FILE=/tmp/ca.pem\n");
    expect_invalid("ACCESS_SERVER_UPSTREAM_TLS_MODE=system_ca\n"
                   "ACCESS_SERVER_UPSTREAM_TLS_CA_FILE=/tmp/ca.pem\n");

    std::string oversized = "ACCESS_SERVER_UPSTREAM_TLS_MODE=custom_ca\n"
                            "ACCESS_SERVER_UPSTREAM_TLS_CA_FILE=";
    oversized.append(4097, 'a');
    oversized.push_back('\n');
    expect_invalid(oversized);

    std::string embedded_nul = "ACCESS_SERVER_UPSTREAM_TLS_MODE=custom_ca\n"
                               "ACCESS_SERVER_UPSTREAM_TLS_CA_FILE=/tmp/ca";
    embedded_nul.push_back('\0');
    embedded_nul.append(".pem\n");
    expect_invalid(embedded_nul);
}

TEST(AccessServerRuntimeTest, RejectsInvalidUpstreamTrustStoreBeforeEventLoopsStart) {
    constexpr std::string_view kMissingCaPath = "/missing/runtime-upstream-ca.pem";
    auto config = AccessServerConfig::load_from_string(
            "NACOS_SERVER_ADDRESSES=127.0.0.1\n"
            "ACCESS_SERVER_UPSTREAM_TLS_MODE=custom_ca\n"
            "ACCESS_SERVER_UPSTREAM_TLS_CA_FILE=/missing/runtime-upstream-ca.pem\n");
    ASSERT_TRUE(config) << config.error().detail;

    event::EventLoop accept_loop;
    event::EventLoopGroup http_workers(1);
    event::EventLoopGroup nacos_group(1);
    event::EventLoopGroup compiler_group(1);
    event::EventLoopGroup cat_group(1);
    auto runtime = AccessServerRuntime::create(accept_loop, nacos_group.at(0), compiler_group.at(0), cat_group.at(0),
                                               http_workers, *config);

    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error().code, AccessServerRuntimeErrorCode::InitializeUpstreamTls);
    EXPECT_EQ(runtime.error().io_error, common::IoErr::Invalid);
    EXPECT_EQ(runtime.error().message.find(kMissingCaPath), std::string::npos);
    EXPECT_EQ(access_server_runtime_stage_name(runtime.error().code), "initialize upstream TLS trust store");
}

TEST(AccessServerConfigTest, LoadsOptionalCatClientSettings) {
    auto config = AccessServerConfig::load_from_string(R"(
        NACOS_SERVER_ADDRESSES=127.0.0.1
        CAT_APP_KEY=unified-access-server
        CAT_HOSTNAME=access-0
        CAT_IP=127.0.0.1
        CAT_ROUTER_ADDRESSES=127.0.0.2:8080
        CAT_COLLECTOR_ADDRESSES=127.0.0.3:2280
    )");

    ASSERT_TRUE(config) << config.error().detail;
    ASSERT_TRUE(config->cat_config());
    EXPECT_EQ(config->cat_config()->app_key(), "unified-access-server");
    EXPECT_EQ(config->cat_config()->hostname(), "access-0");
    EXPECT_EQ(config->cat_config()->ip(), "127.0.0.1");
    ASSERT_EQ(config->cat_config()->routers().size(), 1U);
    EXPECT_EQ(config->cat_config()->routers()[0].port, 8080);
    ASSERT_EQ(config->cat_config()->bootstrap_collectors().size(), 1U);
    EXPECT_EQ(config->cat_config()->bootstrap_collectors()[0].port(), 2280);
}

TEST(AccessServerConfigTest, RejectsPartialCatClientSettings) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_TLS_ENABLED=false\n"
                                                       "ACCESS_SERVER_HTTP3_ENABLED=false\n"
                                                       "CAT_APP_KEY=unified-access-server\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
    EXPECT_NE(config.error().detail.find("CAT_HOSTNAME"), std::string::npos);
}

TEST(AccessServerConfigTest, RejectsMissingNacosServerList) {
    auto config = AccessServerConfig::load_from_string("ACCESS_SERVER_TLS_ENABLED=false\n"
                                                       "ACCESS_SERVER_HTTP3_ENABLED=false\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::MissingRequiredKey);
    EXPECT_EQ(config.error().key, "NACOS_SERVER_ADDRESSES");
}

TEST(AccessServerConfigTest, RejectsDuplicateAndUnknownKeys) {
    auto duplicate = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                          "NACOS_SERVER_ADDRESSES=127.0.0.2\n");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, AccessServerConfigErrorCode::DuplicateKey);
    EXPECT_EQ(duplicate.error().line, 2U);

    auto unknown = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                        "ACCESS_SERVER_MAGIC=true\n");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, AccessServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(unknown.error().line, 2U);
}

TEST(AccessServerConfigTest, RejectsRemovedHttpWorkerSetting) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_HTTP_WORKERS=4\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(config.error().key, "ACCESS_SERVER_HTTP_WORKERS");
}

TEST(AccessServerConfigTest, RejectsRemovedDefaultUpstreamClusterSetting) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_DEFAULT_CLUSTER=stable\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(config.error().key, "ACCESS_SERVER_DEFAULT_CLUSTER");
}

TEST(AccessServerConfigTest, RequiresCompleteNacosCredentials) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_TLS_ENABLED=false\n"
                                                       "ACCESS_SERVER_HTTP3_ENABLED=false\n"
                                                       "NACOS_USERNAME=user\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidNacosConfig);
}

TEST(AccessServerConfigTest, RejectsNonBooleanTestMode) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_TEST_MODE=TRUE\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(config.error().key, "ACCESS_SERVER_TEST_MODE");
}

TEST(AccessServerConfigTest, RejectsInvalidAccessLogPolicySettings) {
    const auto expect_invalid = [](std::string_view setting) {
        std::string input = "NACOS_SERVER_ADDRESSES=127.0.0.1\n";
        input.append(setting);
        input.push_back('\n');
        auto config = AccessServerConfig::load_from_string(input);
        EXPECT_FALSE(config);
        if (!config) {
            EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
        }
    };

    expect_invalid("ACCESS_SERVER_ACCESS_LOG_QUERY_ALLOWLIST=page,page");
    expect_invalid("ACCESS_SERVER_ACCESS_LOG_QUERY_ALLOWLIST=page,");
    expect_invalid("ACCESS_SERVER_ACCESS_LOG_QUERY_ALLOWLIST=not allowed");
    expect_invalid("ACCESS_SERVER_ACCESS_LOG_SENSITIVE_QUERY_KEYS=token,Token");
    expect_invalid("ACCESS_SERVER_ACCESS_LOG_QUERY_HASH_ENABLED=yes");
    expect_invalid("ACCESS_SERVER_ACCESS_LOG_SUCCESS_SAMPLE_RATE_BPS=10001");
    expect_invalid("ACCESS_SERVER_ACCESS_LOG_MAX_PATH_BYTES=15");
    expect_invalid("ACCESS_SERVER_ACCESS_LOG_MAX_QUERY_BYTES=65537");
}

TEST(AccessServerConfigTest, LoadsLegacyClientMetadataModeAndRejectsUnsafeProxyConfiguration) {
    auto legacy = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_CLIENT_METADATA_MODE=legacy_headers\n");
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->client_metadata_options().mode, ClientMetadataMode::LegacyHeaders);

    const auto expect_invalid = [](std::string_view settings) {
        std::string input = "NACOS_SERVER_ADDRESSES=127.0.0.1\n";
        input.append(settings);
        auto config = AccessServerConfig::load_from_string(input);
        EXPECT_FALSE(config);
        if (!config) {
            EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
        }
    };
    expect_invalid("ACCESS_SERVER_CLIENT_METADATA_MODE=automatic\n");
    expect_invalid("ACCESS_SERVER_CLIENT_METADATA_MODE=trusted_proxy\n");
    expect_invalid("ACCESS_SERVER_TRUSTED_PROXY_CIDRS=10.0.0.0/8\n");
    expect_invalid("ACCESS_SERVER_CLIENT_METADATA_MODE=trusted_proxy\n"
                   "ACCESS_SERVER_TRUSTED_PROXY_CIDRS=.1.2.3/8\n");
    expect_invalid("ACCESS_SERVER_CLIENT_METADATA_MODE=trusted_proxy\n"
                   "ACCESS_SERVER_TRUSTED_PROXY_CIDRS=10.0.0.0/8,\n");
}

TEST(AccessServerConfigTest, LoadsTlsIdentityFromNacosAndRejectsRemovedFileSettings) {
    auto nacos_identity = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n");
    ASSERT_TRUE(nacos_identity);
    EXPECT_TRUE(nacos_identity->http_server_options().tls.cert_file.empty());
    EXPECT_EQ(nacos_identity->tls_certificate_watcher_options().data_id, "ploto.unified-access.tls-certificates");
    EXPECT_EQ(nacos_identity->tls_certificate_watcher_options().group, "ACCESS-SERVER");

    auto removed_file_setting = AccessServerConfig::load_from_string(
            "NACOS_SERVER_ADDRESSES=127.0.0.1\nACCESS_SERVER_TLS_CERTIFICATE_FILE=/"
            "tmp/cert.pem\n");
    ASSERT_FALSE(removed_file_setting);
    EXPECT_EQ(removed_file_setting.error().code, AccessServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(removed_file_setting.error().key, "ACCESS_SERVER_TLS_CERTIFICATE_FILE");
}

TEST(AccessServerConfigTest, RejectsHttp3WithoutTls) {
    auto config =
            AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\nACCESS_SERVER_TLS_ENABLED=false\n");
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(config.error().key, "ACCESS_SERVER_HTTP3_ENABLED");
}

} // namespace
} // namespace fiber::access_server
