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
                              std::vector<std::string> exclude = {}) {
    return EdgeFunctionMountConfig{
            .projects =
                    EdgeFunctionProjectsSelector{
                            .include = std::move(include),
                            .exclude = std::move(exclude),
                    },
            .priority = priority,
    };
}

EdgeFunctionIndexEntry function(std::string id, std::vector<EdgeFunctionMountConfig> mounts) {
    EdgeFunctionIndexEntry entry;
    entry.id = std::move(id);
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

std::vector<std::string_view> mounted_ids(const EdgeFunctionMountTable &table, std::string_view project) {
    std::vector<std::string_view> ids;
    for (const EdgeFunctionMounted &entry: table.mounted_for(project)) {
        ids.push_back(entry.id);
    }
    return ids;
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
    EXPECT_EQ(mounted_ids(*table, "loan-web"), std::vector<std::string_view>({"waf"}));
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
    EXPECT_EQ(mounted[0].id, "f");
    EXPECT_EQ(mounted[0].priority, 1);
    const std::span<const EdgeFunctionMounted> fallback = table->mounted_for("api");
    ASSERT_EQ(fallback.size(), 1U);
    EXPECT_EQ(fallback[0].priority, 10);
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
    EXPECT_EQ(mounted_ids(*table, "proj"), std::vector<std::string_view>({"b", "a", "c", "d"}));
}

TEST(EdgeFunctionMountTable, PerProjectGroupsAreIndependent) {
    EdgeFunctionsState state =
            state_with({function("shared", {mount({"p-*"}, 0)}), function("only-q", {mount({"q"}, 0)})});
    const std::vector<std::string> projects = {"p-1", "q", "r"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_ids(*table, "p-1"), std::vector<std::string_view>({"shared"}));
    EXPECT_EQ(mounted_ids(*table, "q"), std::vector<std::string_view>({"only-q"}));
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
