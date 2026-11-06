#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/AccessConfig.h"
#include "routing/EdgeFunctionsSnapshot.h"

namespace {

using fiber::access_server::edge_glob_matches;
using fiber::access_server::EdgeFunctionIndexEntry;
using fiber::access_server::EdgeFunctionMountConfig;
using fiber::access_server::EdgeFunctionMounted;
using fiber::access_server::EdgeFunctionMountTable;
using fiber::access_server::EdgeFunctionProgram;
using fiber::access_server::EdgeFunctionProjectsSelector;
using fiber::access_server::EdgeFunctionsState;

EdgeFunctionMountConfig mount(std::vector<std::string> include, std::int32_t priority,
                              std::vector<std::string> exclude = {}, std::int32_t ratio = -1) {
    // ratio < 0 means "leave the struct default" so tests can assert it.
    if (ratio < 0) {
        return EdgeFunctionMountConfig{
                .projects =
                        EdgeFunctionProjectsSelector{
                                .include = std::move(include),
                                .exclude = std::move(exclude),
                        },
                .priority = priority,
        };
    }
    return EdgeFunctionMountConfig{
            .projects =
                    EdgeFunctionProjectsSelector{
                            .include = std::move(include),
                            .exclude = std::move(exclude),
                    },
            .priority = priority,
            .ratio = ratio,
    };
}

EdgeFunctionIndexEntry function(std::string name, std::vector<EdgeFunctionMountConfig> mounts) {
    EdgeFunctionIndexEntry entry;
    entry.name = std::move(name);
    entry.mounts = std::move(mounts);
    return entry;
}

// The table never executes programs, so default-constructed (invalid) scripts
// suffice; each entry keeps a distinct pointer so alignment is assertable.
EdgeFunctionsState state_with(std::vector<EdgeFunctionIndexEntry> entries) {
    EdgeFunctionsState state;
    for (EdgeFunctionIndexEntry &entry: entries) {
        state.programs.push_back(std::make_shared<EdgeFunctionProgram>());
        state.index.functions.push_back(std::move(entry));
    }
    state.total_script_bytes = 128;
    return state;
}

// The table borrows project-name views from the caller's strings, so the
// projects vector must outlive every mounted_for() call; every test below
// keeps a named local.
std::shared_ptr<const EdgeFunctionMountTable> build_table(const EdgeFunctionsState &state,
                                                          const std::vector<std::string> &projects) {
    std::vector<std::string_view> views(projects.begin(), projects.end());
    return EdgeFunctionMountTable::build(state, views);
}

std::vector<std::string_view> mounted_names(const EdgeFunctionMountTable &table, std::string_view project) {
    std::vector<std::string_view> names;
    for (const EdgeFunctionMounted &entry: table.mounted_for(project)) {
        names.push_back(entry.name);
    }
    return names;
}

TEST(EdgeGlob, MatchesSingleStarSemantics) {
    EXPECT_TRUE(edge_glob_matches("*", "anything"));
    EXPECT_TRUE(edge_glob_matches("*", ""));
    EXPECT_TRUE(edge_glob_matches("loan-*", "loan-"));
    EXPECT_TRUE(edge_glob_matches("loan-*", "loan-web-2"));
    EXPECT_FALSE(edge_glob_matches("loan-*", "api"));
    EXPECT_FALSE(edge_glob_matches("loan-*", "xloan-web"));
    EXPECT_TRUE(edge_glob_matches("a*b*c", "aXbYc"));
    EXPECT_TRUE(edge_glob_matches("a*b*c", "abc"));
    EXPECT_FALSE(edge_glob_matches("a*b*c", "abx"));
    // Case-sensitive, no character classes, '*' only.
    EXPECT_FALSE(edge_glob_matches("Loan-*", "loan-web"));
    EXPECT_FALSE(edge_glob_matches("loan.?", "loan-x"));
    EXPECT_TRUE(edge_glob_matches("", ""));
    EXPECT_FALSE(edge_glob_matches("", "x"));
}

TEST(EdgeFunctionMountTable, IncludeAndExcludeGlobs) {
    EdgeFunctionsState state = state_with({function("waf", {mount({"*-web"}, 0, {"internal-*"})})});
    const std::vector<std::string> projects = {"loan-web", "internal-web", "loan-app", "api"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "loan-web"), std::vector<std::string_view>({"waf"}));
    EXPECT_TRUE(table->mounted_for("internal-web").empty());
    EXPECT_TRUE(table->mounted_for("loan-app").empty());
    EXPECT_TRUE(table->mounted_for("api").empty());
    EXPECT_EQ(table->mounted_project_count(), 1U);
}

TEST(EdgeFunctionMountTable, FirstMatchingMountWins) {
    // Declaration order decides: the "loan-*" mount wins there even though the
    // later "*" mount also matches; elsewhere only "*" applies.
    EdgeFunctionsState state = state_with({function("f", {mount({"loan-*"}, 1), mount({"*"}, 10)})});
    const std::vector<std::string> projects = {"loan-web", "api"};
    auto table = build_table(state, projects);
    const std::span<const EdgeFunctionMounted> mounted = table->mounted_for("loan-web");
    ASSERT_EQ(mounted.size(), 1U);
    EXPECT_EQ(mounted[0].name, "f");
    EXPECT_EQ(mounted[0].priority, 1);
    const std::span<const EdgeFunctionMounted> fallback = table->mounted_for("api");
    ASSERT_EQ(fallback.size(), 1U);
    EXPECT_EQ(fallback[0].priority, 10);
}

TEST(EdgeFunctionMountTable, MatchingMountRatioCarriesToMountedEntry) {
    // The winning mount's ratio travels with the mounted entry; a mount that
    // leaves ratio unset stays at the must-run default.
    EdgeFunctionsState state =
            state_with({function("f", {mount({"loan-*"}, 1, {}, 250), mount({"*"}, 10, {}, 10000)})});
    const std::vector<std::string> projects = {"loan-web", "api"};
    auto table = build_table(state, projects);
    EXPECT_EQ(table->mounted_for("loan-web")[0].ratio, 250);
    EXPECT_EQ(table->mounted_for("api")[0].ratio, 10000);

    EdgeFunctionsState defaulted = state_with({function("g", {mount({"*"}, 0)})});
    auto default_table = build_table(defaulted, projects);
    EXPECT_EQ(default_table->mounted_for("api")[0].ratio, fiber::access_server::kEdgeFunctionFullRatio);
}

TEST(EdgeFunctionMountTable, ExcludedFirstMountFallsThroughToSecond) {
    EdgeFunctionsState state =
            state_with({function("f", {mount({"loan-*"}, 1, {"loan-internal"}), mount({"loan-*"}, 7)})});
    const std::vector<std::string> projects = {"loan-web", "loan-internal"};
    auto table = build_table(state, projects);
    EXPECT_EQ(table->mounted_for("loan-web").front().priority, 1);
    EXPECT_EQ(table->mounted_for("loan-internal").front().priority, 7);
}

TEST(EdgeFunctionMountTable, PriorityOrderKeepsDeclarationTies) {
    EdgeFunctionsState state = state_with({function("a", {mount({"*"}, 5)}), function("b", {mount({"*"}, 1)}),
                                           function("c", {mount({"*"}, 5)}), function("d", {mount({"*"}, 9)})});
    const std::vector<std::string> projects = {"proj"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "proj"), std::vector<std::string_view>({"b", "a", "c", "d"}));
}

TEST(EdgeFunctionMountTable, PerProjectGroupsAreIndependent) {
    EdgeFunctionsState state =
            state_with({function("shared", {mount({"p-*"}, 0)}), function("only-q", {mount({"q"}, 0)})});
    const std::vector<std::string> projects = {"p-1", "q", "r"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "p-1"), std::vector<std::string_view>({"shared"}));
    EXPECT_EQ(mounted_names(*table, "q"), std::vector<std::string_view>({"only-q"}));
    EXPECT_TRUE(table->mounted_for("r").empty());
    EXPECT_EQ(table->mounted_project_count(), 2U);
}

TEST(EdgeFunctionMountTable, EmptyIndexUnmountsEverything) {
    EdgeFunctionsState empty_state;
    const std::vector<std::string> projects = {"p1"};
    auto table = build_table(empty_state, projects);
    EXPECT_TRUE(table->empty());
    EXPECT_TRUE(table->mounted_for("p1").empty());
    EXPECT_EQ(table->mounted_project_count(), 0U);
}

TEST(EdgeFunctionMountTable, NoProjectsYieldsEmptyTable) {
    EdgeFunctionsState state = state_with({function("f", {mount({"*"}, 0)})});
    const std::vector<std::string> projects = {};
    auto table = build_table(state, projects);
    EXPECT_TRUE(table->empty());
}

TEST(EdgeFunctionMountTable, ProgramPointersAlignWithIndex) {
    EdgeFunctionsState state = state_with({function("a", {mount({"p"}, 0)}), function("b", {mount({"*"}, 0)})});
    const std::vector<std::string> projects = {"p", "q"};
    auto table = build_table(state, projects);
    const std::span<const EdgeFunctionMounted> p = table->mounted_for("p");
    ASSERT_EQ(p.size(), 2U);
    EXPECT_EQ(p[0].program, state.programs[0].get());
    EXPECT_EQ(p[1].program, state.programs[1].get());
    const std::span<const EdgeFunctionMounted> q = table->mounted_for("q");
    ASSERT_EQ(q.size(), 1U);
    EXPECT_EQ(q[0].program, state.programs[1].get());
}

} // namespace
