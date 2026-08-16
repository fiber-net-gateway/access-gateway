#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "routing/AccessRouteSnapshot.h"
#include "routing/ProjectConfigCompiler.h"
#include "runtime/ProjectSnapshotRegistry.h"
#include "runtime/RouteSnapshotPublisher.h"

namespace {

using fiber::access_server::AccessRouteSnapshot;
using fiber::access_server::BodyType;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ProjectConfigCompiler;
using fiber::access_server::ProjectRouteSnapshot;
using fiber::access_server::ProjectSnapshotRegistry;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteSnapshotPublisher;
using fiber::access_server::RouteType;

std::shared_ptr<const ProjectRouteSnapshot> project_snapshot(std::string project, std::int32_t version,
                                                             std::string host) {
    RouteConfig route;
    route.path = "/";
    route.type = RouteType::Response;
    route.status = 200;
    route.body = RouteBodyConfig{
            .type = BodyType::Text,
            .content = "ok",
    };

    ProjectConfig config;
    config.version = version;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = std::move(host),
                    .strategy = HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    auto compiled = ProjectConfigCompiler{}.compile(project, config);
    if (!compiled || !*compiled) {
        return {};
    }
    return std::make_shared<const ProjectRouteSnapshot>(std::move(**compiled));
}

TEST(RouteSnapshotComponentsTest, KeepsConcreteControlPlaneComponentsBounded) {
    EXPECT_FALSE(std::is_polymorphic_v<ProjectConfigCompiler>);
    EXPECT_FALSE(std::is_polymorphic_v<ProjectSnapshotRegistry>);
    EXPECT_FALSE(std::is_polymorphic_v<RouteSnapshotPublisher>);
    EXPECT_LE(sizeof(ProjectConfigCompiler), 32U);
    EXPECT_LE(sizeof(ProjectSnapshotRegistry), 64U);
    EXPECT_LE(sizeof(RouteSnapshotPublisher), 32U);
}

TEST(RouteSnapshotComponentsTest, RegistryOwnsVersionUnloadRemoveAndOrderingSemantics) {
    ProjectSnapshotRegistry registry;
    auto beta = project_snapshot("beta", 1, "beta.example.com");
    auto alpha = project_snapshot("alpha", 2, "alpha.example.com");
    ASSERT_TRUE(beta);
    ASSERT_TRUE(alpha);

    registry.replace("beta", 1, beta);
    registry.replace("alpha", 2, alpha);
    EXPECT_EQ(registry.current_version("alpha"), 2);
    EXPECT_EQ(registry.current_version("beta"), 1);
    auto loaded = registry.loaded_snapshots();
    ASSERT_EQ(loaded.size(), 2U);
    EXPECT_EQ(loaded[0]->project(), "alpha");
    EXPECT_EQ(loaded[1]->project(), "beta");

    registry.unload("alpha");
    EXPECT_EQ(registry.current_version("alpha"), 2);
    EXPECT_EQ(registry.find_snapshot("alpha"), nullptr);
    loaded = registry.loaded_snapshots();
    ASSERT_EQ(loaded.size(), 1U);
    EXPECT_EQ(loaded[0]->project(), "beta");

    registry.remove("alpha");
    EXPECT_FALSE(registry.current_version("alpha"));
    EXPECT_EQ(registry.find_snapshot("alpha"), nullptr);
    registry.clear();
    EXPECT_TRUE(registry.empty());
}

TEST(RouteSnapshotComponentsTest, PublisherKeepsOldPinsAliveAcrossReleasePublication) {
    RouteSnapshotPublisher publisher;
    const auto initial = publisher.pin();
    ASSERT_TRUE(initial);
    EXPECT_TRUE(initial->projects().empty());

    auto project = project_snapshot("demo", 1, "api.example.com");
    ASSERT_TRUE(project);
    const std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects{project};
    auto built = AccessRouteSnapshot::build(projects);
    ASSERT_TRUE(built);
    auto published = std::make_shared<const AccessRouteSnapshot>(std::move(*built));
    publisher.publish(published);

    const auto current = publisher.pin();
    EXPECT_EQ(current, published);
    EXPECT_TRUE(current->match_host("api.example.com"));
    EXPECT_TRUE(initial->projects().empty());

    publisher.publish(std::make_shared<const AccessRouteSnapshot>());
    EXPECT_TRUE(publisher.pin()->projects().empty());
    EXPECT_TRUE(current->match_host("api.example.com"));
}

} // namespace
