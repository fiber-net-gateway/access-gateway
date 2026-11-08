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
using fiber::access_server::EdgeFunctionMounted;
using fiber::access_server::EdgeFunctionMountTable;
using fiber::access_server::EdgeFunctionProgram;
using fiber::access_server::EdgeFunctionProjectsSelector;
using fiber::access_server::EdgeFunctionsState;

EdgeFunctionIndexEntry function(std::string name, std::vector<std::string> include, std::int32_t priority = 0,
                                std::vector<std::string> exclude = {},
                                std::int32_t ratio = fiber::access_server::kEdgeFunctionFullRatio) {
    EdgeFunctionIndexEntry entry;
    entry.name = std::move(name);
    entry.projects = EdgeFunctionProjectsSelector{.include = std::move(include), .exclude = std::move(exclude)};
    entry.priority = priority;
    entry.ratio = ratio;
    return entry;
}

EdgeFunctionIndexEntry canary(std::string name, std::vector<std::string> include, std::int32_t ratio,
                              std::string replaces) {
    EdgeFunctionIndexEntry entry = function(std::move(name), std::move(include), 0, {}, ratio);
    entry.replaces = std::move(replaces);
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

std::vector<std::string_view> mounted_body_names(const EdgeFunctionMountTable &table, std::string_view project) {
    std::vector<std::string_view> names;
    for (const EdgeFunctionMounted &entry: table.mounted_body_for(project)) {
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
    EdgeFunctionsState state = state_with({function("waf", {"*-web"}, 0, {"internal-*"})});
    const std::vector<std::string> projects = {"loan-web", "internal-web", "loan-app", "api"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "loan-web"), std::vector<std::string_view>({"waf"}));
    EXPECT_TRUE(table->mounted_for("internal-web").empty());
    EXPECT_TRUE(table->mounted_for("loan-app").empty());
    EXPECT_TRUE(table->mounted_for("api").empty());
    EXPECT_EQ(table->mounted_project_count(), 1U);
}

TEST(EdgeFunctionMountTable, RatioBecomesTheBucketWindowEnd) {
    // A plain function owns [0, ratio); the default stays the full window.
    EdgeFunctionsState state = state_with({function("f", {"*"}, 1, {}, 250)});
    const std::vector<std::string> projects = {"api"};
    auto table = build_table(state, projects);
    EXPECT_EQ(table->mounted_for("api")[0].bucket_begin, 0);
    EXPECT_EQ(table->mounted_for("api")[0].bucket_end, 250);
    EXPECT_FALSE(table->mounted_for("api")[0].always_runs());
    EXPECT_FALSE(table->mounted_for("api")[0].never_runs());

    EdgeFunctionsState defaulted = state_with({function("g", {"*"})});
    auto default_table = build_table(defaulted, projects);
    EXPECT_TRUE(default_table->mounted_for("api")[0].always_runs());

    EdgeFunctionsState disabled = state_with({function("h", {"*"}, 0, {}, 0)});
    auto disabled_table = build_table(disabled, projects);
    EXPECT_TRUE(disabled_table->mounted_for("api")[0].never_runs());
}

TEST(EdgeFunctionMountTable, ReplacesSplitsBucketsOnlyWhereBothAreMounted) {
    EdgeFunctionsState state = state_with({function("auth-v1", {"*"}), canary("auth-v2", {"loan-*"}, 300, "auth-v1")});
    const std::vector<std::string> projects = {"loan-web", "api"};
    auto table = build_table(state, projects);

    const std::span<const EdgeFunctionMounted> loan = table->mounted_for("loan-web");
    ASSERT_EQ(loan.size(), 2U);
    EXPECT_EQ(loan[0].name, "auth-v1");
    EXPECT_EQ(loan[0].bucket_begin, 300);
    EXPECT_EQ(loan[0].bucket_end, 10000);
    EXPECT_EQ(loan[1].name, "auth-v2");
    EXPECT_EQ(loan[1].bucket_begin, 0);
    EXPECT_EQ(loan[1].bucket_end, 300);

    // Outside the canary's selector the target keeps everything.
    const std::span<const EdgeFunctionMounted> api = table->mounted_for("api");
    ASSERT_EQ(api.size(), 1U);
    EXPECT_EQ(api[0].name, "auth-v1");
    EXPECT_TRUE(api[0].always_runs());
}

TEST(EdgeFunctionMountTable, ReplacesNeverExtendsTheTargetsReach) {
    // The target's exclude and include stay authoritative: where it is not
    // mounted a canary miss runs nothing, it does not pull the target in.
    EdgeFunctionsState state =
            state_with({function("auth-v1", {"loan-*"}, 0, {"loan-legacy"}), canary("auth-v2", {"*"}, 300, "auth-v1")});
    const std::vector<std::string> projects = {"loan-web", "loan-legacy", "api"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "loan-web"), (std::vector<std::string_view>{"auth-v1", "auth-v2"}));
    EXPECT_EQ(mounted_names(*table, "loan-legacy"), (std::vector<std::string_view>{"auth-v2"}));
    EXPECT_EQ(mounted_names(*table, "api"), (std::vector<std::string_view>{"auth-v2"}));
}

TEST(EdgeFunctionMountTable, ReplacesKeepsEachFunctionsOwnPriority) {
    EdgeFunctionIndexEntry stable = function("auth-v1", {"*"}, 100);
    EdgeFunctionIndexEntry next = canary("auth-v2", {"*"}, 300, "auth-v1");
    next.priority = 5;
    EdgeFunctionsState state = state_with({function("between", {"*"}, 50), stable, next});
    const std::vector<std::string> projects = {"proj"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "proj"), (std::vector<std::string_view>{"auth-v2", "between", "auth-v1"}));
}

TEST(EdgeFunctionMountTable, ReplacesRampEndpointsLeaveOneSideEmpty) {
    const std::vector<std::string> projects = {"proj"};

    // ratio 0: the canary is staged, the target keeps every bucket and no
    // request pays for a draw.
    EdgeFunctionsState staged = state_with({function("auth-v1", {"*"}), canary("auth-v2", {"*"}, 0, "auth-v1")});
    auto staged_table = build_table(staged, projects);
    EXPECT_TRUE(staged_table->mounted_for("proj")[0].always_runs());
    EXPECT_TRUE(staged_table->mounted_for("proj")[1].never_runs());

    // ratio 10000: the target stays mounted for an instant rollback but owns
    // nothing.
    EdgeFunctionsState full = state_with({function("auth-v1", {"*"}), canary("auth-v2", {"*"}, 10000, "auth-v1")});
    auto full_table = build_table(full, projects);
    EXPECT_TRUE(full_table->mounted_for("proj")[0].never_runs());
    EXPECT_TRUE(full_table->mounted_for("proj")[1].always_runs());
}

TEST(EdgeFunctionMountTable, ReplacesRespectsTheTargetsOwnRatio) {
    const std::vector<std::string> projects = {"proj"};

    // A sampled target keeps what is left of its own window.
    EdgeFunctionsState narrowed =
            state_with({function("auth-v1", {"*"}, 0, {}, 5000), canary("auth-v2", {"*"}, 300, "auth-v1")});
    auto narrowed_table = build_table(narrowed, projects);
    EXPECT_EQ(narrowed_table->mounted_for("proj")[0].bucket_begin, 300);
    EXPECT_EQ(narrowed_table->mounted_for("proj")[0].bucket_end, 5000);

    // A canary wider than the target's window leaves it nothing.
    EdgeFunctionsState covered =
            state_with({function("auth-v1", {"*"}, 0, {}, 200), canary("auth-v2", {"*"}, 300, "auth-v1")});
    auto covered_table = build_table(covered, projects);
    EXPECT_TRUE(covered_table->mounted_for("proj")[0].never_runs());
}

TEST(EdgeFunctionMountTable, ReplacesAppliesInsideTheBodyChain) {
    EdgeFunctionIndexEntry stable = function("scan-v1", {"*"});
    stable.phase = fiber::access_server::EdgeFunctionPhase::RequestBody;
    EdgeFunctionIndexEntry next = canary("scan-v2", {"*"}, 300, "scan-v1");
    next.phase = fiber::access_server::EdgeFunctionPhase::RequestBody;
    EdgeFunctionsState state = state_with({stable, next});
    const std::vector<std::string> projects = {"proj"};
    auto table = build_table(state, projects);
    const std::span<const EdgeFunctionMounted> body = table->mounted_body_for("proj");
    ASSERT_EQ(body.size(), 2U);
    EXPECT_EQ(body[0].bucket_begin, 300);
    EXPECT_EQ(body[1].bucket_end, 300);
}

TEST(EdgeFunctionMountTable, PriorityOrderKeepsDeclarationTies) {
    EdgeFunctionsState state = state_with(
            {function("a", {"*"}, 5), function("b", {"*"}, 1), function("c", {"*"}, 5), function("d", {"*"}, 9)});
    const std::vector<std::string> projects = {"proj"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "proj"), std::vector<std::string_view>({"b", "a", "c", "d"}));
}

TEST(EdgeFunctionMountTable, PerProjectGroupsAreIndependent) {
    EdgeFunctionsState state = state_with({function("shared", {"p-*"}, 0), function("only-q", {"q"}, 0)});
    const std::vector<std::string> projects = {"p-1", "q", "r"};
    auto table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "p-1"), std::vector<std::string_view>({"shared"}));
    EXPECT_EQ(mounted_names(*table, "q"), std::vector<std::string_view>({"only-q"}));
    EXPECT_TRUE(table->mounted_for("r").empty());
    EXPECT_EQ(table->mounted_project_count(), 2U);
}

TEST(EdgeFunctionMountTable, BodyPhaseEntriesLandInTheBodyChain) {
    EdgeFunctionIndexEntry head = function("waf", {"loan-web"}, 100);
    EdgeFunctionIndexEntry body;
    body.name = "body-scan";
    body.phase = fiber::access_server::EdgeFunctionPhase::RequestBody;
    body.projects.include = {"loan-web"};
    body.priority = 50;
    const EdgeFunctionsState state = state_with({head, body});
    const std::vector<std::string> projects{"loan-web"};
    const std::shared_ptr table = build_table(state, projects);
    EXPECT_EQ(mounted_names(*table, "loan-web"), (std::vector<std::string_view>{"waf"}));
    EXPECT_EQ(mounted_body_names(*table, "loan-web"), (std::vector<std::string_view>{"body-scan"}));
}

TEST(EdgeFunctionMountTable, BodyChainSortsByPriorityIndependently) {
    EdgeFunctionIndexEntry low;
    low.name = "body-low";
    low.phase = fiber::access_server::EdgeFunctionPhase::RequestBody;
    low.projects.include = {"loan-web"};
    low.priority = 200;
    EdgeFunctionIndexEntry high;
    high.name = "body-high";
    high.phase = fiber::access_server::EdgeFunctionPhase::RequestBody;
    high.projects.include = {"loan-web"};
    high.priority = 10;
    EdgeFunctionIndexEntry head_between;
    head_between.name = "head-between";
    head_between.projects.include = {"loan-web"};
    head_between.priority = 100;
    const EdgeFunctionsState state = state_with({low, high, head_between});
    const std::vector<std::string> projects{"loan-web"};
    const std::shared_ptr table = build_table(state, projects);
    // Head and body chains sort independently; a head function's priority
    // never interleaves with the body chain.
    EXPECT_EQ(mounted_names(*table, "loan-web"), (std::vector<std::string_view>{"head-between"}));
    EXPECT_EQ(mounted_body_names(*table, "loan-web"), (std::vector<std::string_view>{"body-high", "body-low"}));
}

TEST(EdgeFunctionMountTable, BodyOnlyProjectStillExposesEmptyHeadChain) {
    EdgeFunctionIndexEntry body;
    body.name = "body-scan";
    body.phase = fiber::access_server::EdgeFunctionPhase::RequestBody;
    body.projects.include = {"trade-web"};
    body.priority = 0;
    const EdgeFunctionsState state = state_with({body});
    const std::vector<std::string> projects{"trade-web"};
    const std::shared_ptr table = build_table(state, projects);
    EXPECT_TRUE(mounted_names(*table, "trade-web").empty());
    EXPECT_EQ(mounted_body_names(*table, "trade-web"), (std::vector<std::string_view>{"body-scan"}));
    // Unknown projects see neither chain.
    EXPECT_TRUE(mounted_body_names(*table, "loan-web").empty());
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
    EdgeFunctionsState state = state_with({function("f", {"*"}, 0)});
    const std::vector<std::string> projects = {};
    auto table = build_table(state, projects);
    EXPECT_TRUE(table->empty());
}

TEST(EdgeFunctionMountTable, ProgramPointersAlignWithIndex) {
    EdgeFunctionsState state = state_with({function("a", {"p"}, 0), function("b", {"*"}, 0)});
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
