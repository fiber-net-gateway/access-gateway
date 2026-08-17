#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/AccessConfigCodec.h"
#include "config/AccessConfigLimits.h"
#include "routing/ProjectConfigCompiler.h"

namespace {

using fiber::access_server::AccessConfigErrorCode;
using fiber::access_server::AccessConfigLimits;
using fiber::access_server::BodyType;
using fiber::access_server::compile_project_config;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::kAccessConfigLimits;
using fiber::access_server::parse_gray_match_config;
using fiber::access_server::parse_project_config;
using fiber::access_server::parse_project_list;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ProxyAddressSelectorFactory;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteType;
using fiber::access_server::ScriptCompilerAdapter;

RouteConfig response_route(std::string path, std::optional<RouteBodyConfig> body = std::nullopt) {
    RouteConfig route;
    route.path = std::move(path);
    route.type = RouteType::Response;
    route.status = 200;
    route.body = std::move(body);
    return route;
}

ProjectConfig project(std::vector<RouteConfig> routes) {
    ProjectConfig config;
    config.hosts = std::vector<HostConfigEntry>{HostConfigEntry{
            .pattern = "example.com",
            .strategy = HostStrategyConfig{},
    }};
    config.routes.emplace();
    config.routes->reserve(routes.size());
    for (RouteConfig &route: routes) {
        config.routes->emplace_back(std::move(route));
    }
    return config;
}

TEST(AccessConfigLimitsTest, RejectsPayloadAndContainerLimitsBeforeModelConstruction) {
    AccessConfigLimits limits = kAccessConfigLimits;
    limits.project_route.max_payload_bytes = 8;
    auto payload = parse_project_config(R"({"routes":[]})", limits);
    ASSERT_FALSE(payload);
    EXPECT_EQ(payload.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(payload.error().field, "payload");

    limits = kAccessConfigLimits;
    limits.project_route.max_routes = 1;
    auto routes = parse_project_config(R"({"routes":[{},{}]})", limits);
    ASSERT_FALSE(routes);
    EXPECT_EQ(routes.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(routes.error().field, "routes");

    limits = kAccessConfigLimits;
    limits.project_route.max_header_entries = 1;
    auto headers = parse_project_config(R"({"routes":[{"proxy_headers":{"A":"1","B":"2"}}]})", limits);
    ASSERT_FALSE(headers);
    EXPECT_EQ(headers.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(headers.error().field, "routes[0].proxy_headers");
}

TEST(AccessConfigLimitsTest, AppliesByteLimitsAfterJavaScalarCoercion) {
    AccessConfigLimits limits = kAccessConfigLimits;
    limits.project_route.max_path_bytes = 3;
    auto path = parse_project_config(R"({"routes":[{"path":"four"}]})", limits);
    ASSERT_FALSE(path);
    EXPECT_EQ(path.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(path.error().field, "routes[0].path");

    limits = kAccessConfigLimits;
    limits.project_route.max_header_value_bytes = 2;
    auto numeric = parse_project_config(R"({"routes":[{"proxy_headers":{"X":123}}]})", limits);
    ASSERT_FALSE(numeric);
    EXPECT_EQ(numeric.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(numeric.error().field, "routes[0].proxy_headers[0].value");

    limits = kAccessConfigLimits;
    limits.project_route.max_cluster_bytes = 3;
    auto service_cluster = parse_project_config(R"({"routes":[{"service":"users/gray"}]})", limits);
    ASSERT_FALSE(service_cluster);
    EXPECT_EQ(service_cluster.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(service_cluster.error().field, "routes[0].service");

    limits = kAccessConfigLimits;
    limits.project_route.max_upstream_tls_ca_pem_bytes = 3;
    auto ca_pem = parse_project_config(
            R"({"routes":[{"upstream_tls":{"generation":1,"verification":"CUSTOM_CA","ca_pem":"four"}}]})", limits);
    ASSERT_FALSE(ca_pem);
    EXPECT_EQ(ca_pem.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(ca_pem.error().field, "routes[0].upstream_tls.ca_pem");

    limits = kAccessConfigLimits;
    limits.project_route.max_upstream_tls_profiles = 1;
    auto profiles = parse_project_config(
            R"({"routes":[{"upstream_tls":{"generation":1}},{"upstream_tls":{"generation":2}}]})", limits);
    ASSERT_FALSE(profiles);
    EXPECT_EQ(profiles.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(profiles.error().field, "routes[1].upstream_tls");
}

TEST(AccessConfigLimitsTest, BoundsProjectListWithoutChangingValidJavaSplitSemantics) {
    AccessConfigLimits limits = kAccessConfigLimits;
    limits.project_list.max_payload_bytes = 5;
    limits.project_list.max_projects = 2;
    limits.project_list.max_project_name_bytes = 3;

    auto valid = parse_project_list("a;b", limits);
    ASSERT_TRUE(valid);
    EXPECT_EQ(*valid, (std::vector<std::string>{"a", "b"}));

    auto trailing = parse_project_list("a;b;", limits);
    ASSERT_TRUE(trailing);
    EXPECT_EQ(*trailing, (std::vector<std::string>{"a", "b"}));

    auto count = parse_project_list("a;b;c", limits);
    ASSERT_FALSE(count);
    EXPECT_EQ(count.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(count.error().field, "projects");

    auto name = parse_project_list("abcd", limits);
    ASSERT_FALSE(name);
    EXPECT_EQ(name.error().field, "projects[0]");

    auto bytes = parse_project_list("abcdef", limits);
    ASSERT_FALSE(bytes);
    EXPECT_EQ(bytes.error().field, "projects");
}

TEST(AccessConfigLimitsTest, EnforcesTemplateProgramAndPathVariableBudgetsDuringCompilation) {
    RouteBodyConfig template_body{
            .type = BodyType::Template,
            .content = "${1}${2}",
    };
    ProjectConfig config = project({response_route("/template", template_body)});

    AccessConfigLimits limits = kAccessConfigLimits;
    limits.project_route.max_template_expressions = 1;
    auto expressions =
            compile_project_config("example", config, ScriptCompilerAdapter{}, ProxyAddressSelectorFactory{}, limits);
    ASSERT_FALSE(expressions);
    EXPECT_EQ(expressions.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(expressions.error().field, "routes[0].body");

    limits = kAccessConfigLimits;
    limits.project_route.max_compiled_programs = 1;
    auto programs =
            compile_project_config("example", config, ScriptCompilerAdapter{}, ProxyAddressSelectorFactory{}, limits);
    ASSERT_FALSE(programs);
    EXPECT_EQ(programs.error().code, AccessConfigErrorCode::LimitExceeded);

    config = project({response_route("/:first/:second")});
    limits = kAccessConfigLimits;
    limits.project_route.max_path_variables = 1;
    auto variables =
            compile_project_config("example", config, ScriptCompilerAdapter{}, ProxyAddressSelectorFactory{}, limits);
    ASSERT_FALSE(variables);
    EXPECT_EQ(variables.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(variables.error().field, "routes[0].path");
}

TEST(AccessConfigLimitsTest, EnforcesStaticResponseAndEstimatedSnapshotBudgets) {
    RouteBodyConfig body{
            .type = BodyType::Text,
            .content = "aa",
    };
    ProjectConfig config = project({response_route("/a", body), response_route("/b", body)});

    AccessConfigLimits limits = kAccessConfigLimits;
    limits.project_route.max_static_response_body_bytes = 10;
    limits.project_route.max_static_response_bytes = 3;
    auto bodies =
            compile_project_config("example", config, ScriptCompilerAdapter{}, ProxyAddressSelectorFactory{}, limits);
    ASSERT_FALSE(bodies);
    EXPECT_EQ(bodies.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(bodies.error().field, "routes[1].body");

    limits = kAccessConfigLimits;
    limits.project_route.max_estimated_snapshot_bytes = 1;
    auto memory =
            compile_project_config("example", config, ScriptCompilerAdapter{}, ProxyAddressSelectorFactory{}, limits);
    ASSERT_FALSE(memory);
    EXPECT_EQ(memory.error().code, AccessConfigErrorCode::LimitExceeded);
}

TEST(AccessConfigLimitsTest, BoundsGrayPayloadRulesAndCidrs) {
    AccessConfigLimits limits = kAccessConfigLimits;
    limits.gray_rules.max_rules = 1;
    auto rules = parse_gray_match_config(R"({"vdi":{},"desktop":{}})", limits);
    ASSERT_FALSE(rules);
    EXPECT_EQ(rules.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(rules.error().field, "rules");

    limits = kAccessConfigLimits;
    limits.gray_rules.max_cidrs_per_rule = 1;
    auto cidrs = parse_gray_match_config(R"({"vdi":{"cidrs":["10.0.0.0/8","192.0.2.0/24"]}})", limits);
    ASSERT_FALSE(cidrs);
    EXPECT_EQ(cidrs.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(cidrs.error().field, "vdi.cidrs");
}

} // namespace
