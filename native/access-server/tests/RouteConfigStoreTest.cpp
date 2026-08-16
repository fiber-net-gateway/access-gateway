#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/AccessScriptRuntime.h"
#include "runtime/RouteConfigStore.h"

namespace {

using fiber::access_server::AccessScriptRuntime;
using fiber::access_server::BodyType;
using fiber::access_server::compile_project_config;
using fiber::access_server::ConfigUpdateStatus;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::PreparedProjectUpdate;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ReadyProjectUpdate;
using fiber::access_server::ResponseGzipConfig;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteConfigStore;
using fiber::access_server::RouteType;

static_assert(!std::is_default_constructible_v<PreparedProjectUpdate>);
static_assert(!std::is_copy_constructible_v<PreparedProjectUpdate>);
static_assert(std::is_move_constructible_v<PreparedProjectUpdate>);
static_assert(!std::is_default_constructible_v<ReadyProjectUpdate>);
static_assert(!std::is_copy_constructible_v<ReadyProjectUpdate>);
static_assert(std::is_move_constructible_v<ReadyProjectUpdate>);
static_assert(!std::is_invocable_v<decltype(&RouteConfigStore::commit), RouteConfigStore &, PreparedProjectUpdate>);
static_assert(std::is_invocable_v<decltype(&RouteConfigStore::commit), RouteConfigStore &, ReadyProjectUpdate>);

ProjectConfig project_config(std::int32_t version, std::string host, std::string path) {
    RouteConfig route;
    route.path = std::move(path);
    route.service = "service";

    ProjectConfig config;
    config.version = version;
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{
                    .pattern = std::move(host),
                    .strategy = HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<RouteConfig>>{std::move(route)};
    return config;
}

TEST(RouteConfigStoreTest, CommitsAfterCheckedReadyTransition) {
    RouteConfigStore store;
    auto prepared = store.prepare("demo", project_config(1, "one.example.com", "/one"));
    ASSERT_TRUE(prepared);
    EXPECT_TRUE(store.pin()->projects().empty());

    auto ready = std::move(*prepared).try_ready();
    ASSERT_TRUE(ready);
    auto published = store.commit(std::move(*ready));

    ASSERT_TRUE(published);
    EXPECT_EQ(published->status, ConfigUpdateStatus::Published);
    EXPECT_TRUE(store.pin()->match_host("one.example.com"));
}

TEST(RouteConfigStoreTest, BatchCommitsReadyProjectsWithOneAtomicSnapshot) {
    RouteConfigStore store;
    auto before = store.pin();
    std::vector<ReadyProjectUpdate> ready;
    for (const auto &[project, host]: std::vector<std::pair<std::string, std::string>>{
                 {"charlie", "charlie.example.com"},
                 {"alpha", "alpha.example.com"},
                 {"bravo", "bravo.example.com"},
         }) {
        auto prepared = store.prepare(project, project_config(1, host, "/"));
        ASSERT_TRUE(prepared);
        auto candidate = std::move(*prepared).try_ready();
        ASSERT_TRUE(candidate);
        ready.push_back(std::move(*candidate));
    }

    auto committed = store.commit_batch(std::move(ready));

    ASSERT_TRUE(committed);
    EXPECT_TRUE(committed->published);
    ASSERT_EQ(committed->projects.size(), 3U);
    EXPECT_EQ(committed->projects[0].project, "alpha");
    EXPECT_EQ(committed->projects[1].project, "bravo");
    EXPECT_EQ(committed->projects[2].project, "charlie");
    for (const auto &project: committed->projects) {
        ASSERT_TRUE(project.outcome);
        EXPECT_EQ(*project.outcome, ConfigUpdateStatus::Published);
    }
    EXPECT_EQ(store.pin(), committed->snapshot);
    EXPECT_EQ(store.pin()->projects().size(), 3U);
    EXPECT_TRUE(store.pin()->match_host("alpha.example.com"));
    EXPECT_TRUE(store.pin()->match_host("bravo.example.com"));
    EXPECT_TRUE(store.pin()->match_host("charlie.example.com"));
    EXPECT_TRUE(before->projects().empty());
}

TEST(RouteConfigStoreTest, BatchRejectsDuplicateProjectTokensWithoutPublishing) {
    RouteConfigStore store;
    auto before = store.pin();
    std::vector<ReadyProjectUpdate> ready;
    for (const std::int32_t version: {1, 2}) {
        auto prepared = store.prepare("duplicate", project_config(version, "duplicate.example.com", "/"));
        ASSERT_TRUE(prepared);
        auto candidate = std::move(*prepared).try_ready();
        ASSERT_TRUE(candidate);
        ready.push_back(std::move(*candidate));
    }

    auto committed = store.commit_batch(std::move(ready));

    ASSERT_FALSE(committed);
    EXPECT_EQ(committed.error().field, "projects");
    EXPECT_EQ(store.pin(), before);
    EXPECT_TRUE(store.pin()->projects().empty());
    EXPECT_FALSE(store.current_version("duplicate"));
}

TEST(RouteConfigStoreTest, BatchIsolatesConflictingProjectsAndPublishesValidCandidatesOnce) {
    RouteConfigStore store;
    std::vector<ReadyProjectUpdate> ready;
    for (const auto &[project, host]: std::vector<std::pair<std::string, std::string>>{
                 {"right", "shared.example.com"},
                 {"left", "SHARED.EXAMPLE.COM"},
                 {"valid", "valid.example.com"},
         }) {
        auto prepared = store.prepare(project, project_config(1, host, "/"));
        ASSERT_TRUE(prepared);
        auto candidate = std::move(*prepared).try_ready();
        ASSERT_TRUE(candidate);
        ready.push_back(std::move(*candidate));
    }

    auto committed = store.commit_batch(std::move(ready));

    ASSERT_TRUE(committed);
    EXPECT_TRUE(committed->published);
    ASSERT_EQ(committed->projects.size(), 3U);
    ASSERT_TRUE(committed->projects[0].outcome);
    EXPECT_EQ(committed->projects[0].project, "left");
    EXPECT_EQ(*committed->projects[0].outcome, ConfigUpdateStatus::Published);
    EXPECT_EQ(committed->projects[1].project, "right");
    ASSERT_FALSE(committed->projects[1].outcome);
    EXPECT_EQ(committed->projects[1].outcome.error().field, "host");
    ASSERT_TRUE(committed->projects[2].outcome);
    EXPECT_EQ(committed->projects[2].project, "valid");
    EXPECT_EQ(store.pin()->projects().size(), 2U);
    ASSERT_TRUE(store.pin()->match_host("shared.example.com"));
    EXPECT_EQ(store.pin()->match_host("shared.example.com").project->project(), "left");
    EXPECT_TRUE(store.pin()->match_host("valid.example.com"));
    EXPECT_FALSE(store.current_version("right"));
}

TEST(RouteConfigStoreTest, BatchConflictRetainsTheProjectsPreviousSnapshot) {
    RouteConfigStore store;
    ASSERT_TRUE(store.apply("alpha", project_config(1, "alpha.example.com", "/alpha")));
    ASSERT_TRUE(store.apply("beta", project_config(1, "beta.example.com", "/beta")));
    const auto before = store.pin();
    std::vector<ReadyProjectUpdate> ready;
    for (const auto &[project, config]: std::vector<std::pair<std::string, ProjectConfig>>{
                 {"alpha", project_config(2, "BETA.example.com", "/replacement")},
                 {"gamma", project_config(1, "gamma.example.com", "/gamma")},
         }) {
        auto prepared = store.prepare(project, config);
        ASSERT_TRUE(prepared);
        auto candidate = std::move(*prepared).try_ready();
        ASSERT_TRUE(candidate);
        ready.push_back(std::move(*candidate));
    }

    auto committed = store.commit_batch(std::move(ready));

    ASSERT_TRUE(committed);
    EXPECT_TRUE(committed->published);
    ASSERT_EQ(committed->projects.size(), 2U);
    EXPECT_EQ(committed->projects[0].project, "alpha");
    EXPECT_FALSE(committed->projects[0].outcome);
    EXPECT_EQ(committed->projects[1].project, "gamma");
    EXPECT_TRUE(committed->projects[1].outcome);
    EXPECT_EQ(store.current_version("alpha"), 1);
    EXPECT_TRUE(store.pin()->match_host("alpha.example.com"));
    ASSERT_TRUE(store.pin()->match_host("beta.example.com"));
    EXPECT_EQ(store.pin()->match_host("beta.example.com").project->project(), "beta");
    EXPECT_TRUE(store.pin()->match_host("gamma.example.com"));
    EXPECT_EQ(before->projects().size(), 2U);
    EXPECT_FALSE(before->match_host("gamma.example.com"));
}

TEST(RouteConfigStoreTest, BatchUnloadRetainsLastSuccessfulVersion) {
    RouteConfigStore store;
    ASSERT_TRUE(store.apply("demo", project_config(7, "api.example.com", "/one")));
    ProjectConfig unload;
    unload.version = 8;
    unload.hosts = std::vector<HostConfigEntry>{};
    auto prepared = store.prepare("demo", unload);
    ASSERT_TRUE(prepared);
    auto candidate = std::move(*prepared).try_ready();
    ASSERT_TRUE(candidate);
    std::vector<ReadyProjectUpdate> ready;
    ready.push_back(std::move(*candidate));

    auto committed = store.commit_batch(std::move(ready));

    ASSERT_TRUE(committed);
    EXPECT_TRUE(committed->published);
    ASSERT_EQ(committed->projects.size(), 1U);
    ASSERT_TRUE(committed->projects[0].outcome);
    EXPECT_EQ(*committed->projects[0].outcome, ConfigUpdateStatus::Unloaded);
    EXPECT_TRUE(store.pin()->projects().empty());
    EXPECT_EQ(store.current_version("demo"), 7);
    auto same = store.apply("demo", project_config(7, "new.example.com", "/two"));
    ASSERT_TRUE(same);
    EXPECT_EQ(same->status, ConfigUpdateStatus::VersionUnchanged);
}

TEST(RouteConfigStoreTest, RejectsMismatchedCompiledCandidateBeforeTypestateTransition) {
    RouteConfigStore store;
    ProjectConfig config = project_config(7, "one.example.com", "/one");
    auto compiled = compile_project_config("source", config);
    ASSERT_TRUE(compiled);
    ASSERT_TRUE(*compiled);

    auto wrong_project = store.prepare_compiled("target", 7, std::move(**compiled));
    ASSERT_FALSE(wrong_project);
    EXPECT_EQ(wrong_project.error().field, "project");
    EXPECT_TRUE(store.pin()->projects().empty());

    compiled = compile_project_config("source", config);
    ASSERT_TRUE(compiled);
    ASSERT_TRUE(*compiled);
    auto wrong_version = store.prepare_compiled("source", 8, std::move(**compiled));
    ASSERT_FALSE(wrong_version);
    EXPECT_EQ(wrong_version.error().field, "version");
    EXPECT_TRUE(store.pin()->projects().empty());
}

TEST(RouteConfigStoreTest, PublishesCompleteSnapshotsAndKeepsPinnedOldVersion) {
    RouteConfigStore store;
    auto initial_pin = store.pin();
    EXPECT_TRUE(initial_pin->projects().empty());

    auto first = store.apply("demo", project_config(1, "one.example.com", "/one"));
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->status, ConfigUpdateStatus::Published);
    auto first_pin = store.pin();
    ASSERT_TRUE(first_pin->match_host("one.example.com"));

    auto second = store.apply("demo", project_config(2, "two.example.com", "/two"));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second->status, ConfigUpdateStatus::Published);
    auto second_pin = store.pin();
    EXPECT_FALSE(second_pin->match_host("one.example.com"));
    EXPECT_TRUE(second_pin->match_host("two.example.com"));

    EXPECT_TRUE(first_pin->match_host("one.example.com"));
    EXPECT_FALSE(first_pin->match_host("two.example.com"));
    EXPECT_TRUE(initial_pin->projects().empty());
}

TEST(RouteConfigStoreTest, IgnoresEmptyAndSameSuccessfulVersion) {
    RouteConfigStore store;
    ASSERT_TRUE(store.apply("demo", project_config(3, "one.example.com", "/one")));
    auto before = store.pin();

    auto empty = store.apply("demo", std::nullopt);
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, ConfigUpdateStatus::IgnoredEmpty);
    EXPECT_EQ(store.pin(), before);

    auto same = store.apply("demo", project_config(3, "two.example.com", "/two"));
    ASSERT_TRUE(same);
    EXPECT_EQ(same->status, ConfigUpdateStatus::VersionUnchanged);
    EXPECT_EQ(store.pin(), before);
    EXPECT_TRUE(store.pin()->match_host("one.example.com"));
}

TEST(RouteConfigStoreTest, RejectsCandidateWithoutReplacingPublishedSnapshot) {
    RouteConfigStore store;
    ASSERT_TRUE(store.apply("left", project_config(1, "api.example.com", "/left")));
    auto before = store.pin();

    auto duplicate = store.apply("right", project_config(1, "API.EXAMPLE.COM", "/right"));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(store.pin(), before);
    ASSERT_TRUE(store.pin()->match_host("api.example.com"));
    EXPECT_EQ(store.pin()->match_host("api.example.com").project->project(), "left");

    ProjectConfig invalid = project_config(2, "left.example.com", "/bad");
    (*invalid.routes)[0]->service = "";
    auto rejected = store.apply("left", invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(store.pin(), before);
}

TEST(RouteConfigStoreTest, RejectsInvalidLocalScriptWithoutReplacingPublishedSnapshot) {
    AccessScriptRuntime scripts;
    RouteConfigStore store(scripts.compiler_adapter());
    ASSERT_TRUE(store.apply("demo", project_config(1, "api.example.com", "/one")));
    auto before = store.pin();

    ProjectConfig invalid = project_config(2, "new.example.com", "/two");
    (*invalid.routes)[0]->condition = "$path.missing ===";
    auto rejected = store.apply("demo", invalid);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().field, "routes[0].condition");
    EXPECT_EQ(store.pin(), before);
    EXPECT_TRUE(store.pin()->match_host("api.example.com"));
    EXPECT_FALSE(store.pin()->match_host("new.example.com"));
}

TEST(RouteConfigStoreTest, RejectsInvalidGzipCandidateWithoutReplacingPublishedSnapshot) {
    RouteConfigStore store;
    ASSERT_TRUE(store.apply("demo", project_config(1, "api.example.com", "/one")));
    auto before = store.pin();

    ProjectConfig invalid = project_config(2, "new.example.com", "/compressed");
    RouteConfig &route = *(*invalid.routes)[0];
    route.type = RouteType::Response;
    route.service.reset();
    route.status = 200;
    route.body = RouteBodyConfig{.type = BodyType::Template, .content = "dynamic"};
    route.gzip = ResponseGzipConfig{.enabled = true};

    auto rejected = store.apply("demo", invalid);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().field, "routes[0].gzip");
    EXPECT_EQ(store.pin(), before);
    EXPECT_TRUE(store.pin()->match_host("api.example.com"));
    EXPECT_FALSE(store.pin()->match_host("new.example.com"));
}

TEST(RouteConfigStoreTest, RejectsInvalidJavaScriptRouteWithoutReplacingPublishedSnapshot) {
    AccessScriptRuntime scripts;
    RouteConfigStore store(scripts.compiler_adapter());
    ASSERT_TRUE(store.apply("demo", project_config(1, "api.example.com", "/one")));
    auto before = store.pin();

    ProjectConfig invalid = project_config(2, "new.example.com", "/script");
    RouteConfig &route = *(*invalid.routes)[0];
    route.type = RouteType::Script;
    route.service.reset();
    route.script = "return {broken: ;";
    auto rejected = store.apply("demo", invalid);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().field, "routes[0].script");
    EXPECT_EQ(store.pin(), before);
    EXPECT_TRUE(store.pin()->match_host("api.example.com"));
    EXPECT_FALSE(store.pin()->match_host("new.example.com"));
}

TEST(RouteConfigStoreTest, RejectsResponseSideEffectsInRouteExpressions) {
    AccessScriptRuntime scripts;
    RouteConfigStore store(scripts.compiler_adapter());
    ProjectConfig invalid = project_config(1, "api.example.com", "/side-effect");
    (*invalid.routes)[0]->condition = "resp.setHeader('X-Leak', 'true') == null";

    auto rejected = store.apply("demo", invalid);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().field, "routes[0].condition");
    EXPECT_TRUE(store.pin()->projects().empty());
}

TEST(RouteConfigStoreTest, RejectsScriptedRouteWithoutCompiler) {
    RouteConfigStore store;
    ProjectConfig invalid = project_config(1, "api.example.com", "/scripted");
    (*invalid.routes)[0]->condition = "true";

    auto rejected = store.apply("demo", invalid);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().field, "routes[0].condition");
    EXPECT_TRUE(store.pin()->projects().empty());
}

TEST(RouteConfigStoreTest, UnloadRetainsLastSuccessfulVersionUntilProjectRemoval) {
    RouteConfigStore store;
    ASSERT_TRUE(store.apply("demo", project_config(7, "api.example.com", "/one")));

    ProjectConfig unload;
    unload.version = 8;
    unload.hosts = std::vector<HostConfigEntry>{};
    auto unloaded = store.apply("demo", unload);
    ASSERT_TRUE(unloaded);
    EXPECT_EQ(unloaded->status, ConfigUpdateStatus::Unloaded);
    EXPECT_FALSE(store.pin()->match_host("api.example.com"));

    auto old_version = store.apply("demo", project_config(7, "new.example.com", "/two"));
    ASSERT_TRUE(old_version);
    EXPECT_EQ(old_version->status, ConfigUpdateStatus::VersionUnchanged);
    EXPECT_FALSE(store.pin()->match_host("new.example.com"));

    auto removed = store.remove_project("demo");
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->status, ConfigUpdateStatus::ProjectRemoved);

    auto readded = store.apply("demo", project_config(7, "new.example.com", "/two"));
    ASSERT_TRUE(readded);
    EXPECT_EQ(readded->status, ConfigUpdateStatus::Published);
    EXPECT_TRUE(store.pin()->match_host("new.example.com"));
}

} // namespace
