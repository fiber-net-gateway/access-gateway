#include "observability/AccessActivationEvidence.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

TEST(AccessActivationEvidenceTest, PublishesCompleteImmutableSnapshotsAcrossComponents) {
    event::EventLoop loop;
    AccessActivationEvidenceStore store(loop, AccessActivationEvidenceIdentity{
                                                      .instance_id = "access-0",
                                                      .build_version = "test",
                                                      .build_revision = "revision",
                                                      .started_at_unix_millis = 1000,
                                              });
    const AccessRouteActivationEvidenceObserver route_observer = store.route_observer();
    const AccessGrayActivationEvidenceObserver gray_observer = store.gray_observer();
    const AccessEdgeFunctionsActivationEvidenceObserver edge_observer = store.edge_functions_observer();
    std::shared_ptr<const AccessActivationEvidenceSnapshot> route_snapshot;
    std::shared_ptr<const AccessActivationEvidenceSnapshot> gray_snapshot;
    std::shared_ptr<const AccessActivationEvidenceSnapshot> edge_snapshot;

    async::spawn(loop, [&]() -> async::DetachedTask {
        AccessRouteActivationEvidence route;
        route.watcher_state = "running";
        route.readiness_state = "ready";
        route.project_list.active_md5 = "11111111111111111111111111111111";
        route.snapshot_generation = 4;
        route.snapshot_published_at_unix_millis = 2000;
        route.projects.push_back(AccessActivationProjectEvidence{
                .name = "example.com",
                .data_id = "ploto.unified-access.route.example.com",
                .group = "ACCESS-SERVER",
                .subscription_state = "subscribed",
                .candidate_status = AccessActivationCandidateStatus::Accepted,
                .observed_md5 = "22222222222222222222222222222222",
                .observed_version = 9,
                .active_md5 = "22222222222222222222222222222222",
                .active_version = 9,
                .active_snapshot_generation = 4,
                .active_loaded = true,
                .observed_at_unix_millis = 1900,
                .active_at_unix_millis = 2000,
        });
        route_observer.on_update(route_observer.context, route);
        route_snapshot = store.pin();

        AccessGrayActivationEvidence gray;
        gray.watcher_state = "running";
        gray.resource.active_md5 = "33333333333333333333333333333333";
        gray.generation = 2;
        gray.rule_count = 3;
        gray_observer.on_update(gray_observer.context, gray);
        gray_snapshot = store.pin();

        AccessEdgeFunctionsActivationEvidence edge;
        edge.watcher_state = "running";
        edge.index.data_id = "ploto.unified-access.edge-functions";
        edge.index.group = "ACCESS-SERVER";
        edge.index.candidate_status = AccessActivationCandidateStatus::Accepted;
        edge.index.active_md5 = "44444444444444444444444444444444";
        edge.generation = 5;
        edge.function_count = 2;
        edge.total_script_bytes = 512;
        edge.active_index_sha256 = std::string(64, 'a');
        edge_observer.on_update(edge_observer.context, edge);
        edge_snapshot = store.pin();
        loop.stop();
        co_return;
    });
    loop.run();

    ASSERT_TRUE(route_snapshot);
    ASSERT_TRUE(gray_snapshot);
    ASSERT_TRUE(edge_snapshot);
    EXPECT_EQ(route_snapshot->revision, 2U);
    EXPECT_EQ(gray_snapshot->revision, 3U);
    EXPECT_EQ(edge_snapshot->revision, 4U);
    EXPECT_EQ(route_snapshot->gray.generation, 0U);
    EXPECT_EQ(gray_snapshot->gray.generation, 2U);
    EXPECT_EQ(route_snapshot->edge_functions.function_count, 0U);
    EXPECT_EQ(gray_snapshot->edge_functions.function_count, 0U);
    EXPECT_EQ(edge_snapshot->edge_functions.watcher_state, "running");
    EXPECT_EQ(edge_snapshot->edge_functions.index.candidate_status, AccessActivationCandidateStatus::Accepted);
    EXPECT_EQ(edge_snapshot->edge_functions.index.active_md5, "44444444444444444444444444444444");
    EXPECT_EQ(edge_snapshot->edge_functions.generation, 5U);
    EXPECT_EQ(edge_snapshot->edge_functions.function_count, 2U);
    EXPECT_EQ(edge_snapshot->edge_functions.total_script_bytes, 512U);
    EXPECT_EQ(edge_snapshot->edge_functions.active_index_sha256, std::string(64, 'a'));
    EXPECT_EQ(route_snapshot->route.projects.size(), 1U);
    EXPECT_EQ(gray_snapshot->route.projects.size(), 1U);
    EXPECT_EQ(route_snapshot->route_snapshot_fingerprint_sha256, gray_snapshot->route_snapshot_fingerprint_sha256);
    EXPECT_EQ(gray_snapshot->route_snapshot_fingerprint_sha256.size(), 64U);
    EXPECT_EQ(gray_snapshot->identity.instance_id, "access-0");
}

} // namespace
} // namespace fiber::access_server
