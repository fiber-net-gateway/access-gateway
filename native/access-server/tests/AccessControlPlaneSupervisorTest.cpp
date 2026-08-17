#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>
#include <fiber/nacos/Subscription.h>

#include "NacosSnapshotTestBuilder.h"
#include "runtime/AccessControlPlaneSupervisor.h"

namespace {

using fiber::access_server::AccessControlPlaneDependencies;
using fiber::access_server::AccessControlPlaneOptions;
using fiber::access_server::AccessControlPlaneSupervisor;
using fiber::access_server::AccessControlResourceLifecycle;
using fiber::access_server::AccessServerRuntimeErrorCode;

class LifecycleEvents final {
public:
    void add(std::string event) {
        std::lock_guard guard(mutex_);
        events_.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<std::string> snapshot() const {
        std::lock_guard guard(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
};

struct FakeResourceLifecycle {
    LifecycleEvents *events = nullptr;
    std::string name;
    bool fail_start = false;

    [[nodiscard]] AccessControlResourceLifecycle adapter() noexcept {
        return AccessControlResourceLifecycle{
                .context = this,
                .start = &start,
                .shutdown = &shutdown,
        };
    }

    static fiber::common::IoResult<void> start(void *context) noexcept {
        auto &self = *static_cast<FakeResourceLifecycle *>(context);
        self.events->add(self.name + ".start");
        if (self.fail_start) {
            return std::unexpected(fiber::common::IoErr::NotConnected);
        }
        return {};
    }

    static fiber::async::Task<void> shutdown(void *context) noexcept {
        auto &self = *static_cast<FakeResourceLifecycle *>(context);
        self.events->add(self.name + ".stop");
        co_return;
    }
};

enum class ProjectListReplay : std::uint8_t {
    NotFound,
    Closed,
};

class FakeConfigService final : public fiber::nacos::ConfigService {
public:
    FakeConfigService(LifecycleEvents &events, bool fail_start, std::string failed_subscription,
                      ProjectListReplay project_list_replay) :
        events_(&events), fail_start_(fail_start), failed_subscription_(std::move(failed_subscription)),
        project_list_replay_(project_list_replay) {}

    fiber::common::IoResult<void> start() noexcept override {
        events_->add("config.start");
        if (fail_start_) {
            return std::unexpected(fiber::common::IoErr::NotConnected);
        }
        return {};
    }

    fiber::async::Task<void> shutdown() noexcept override {
        events_->add("config.stop");
        co_return;
    }

    fiber::async::Task<std::expected<std::shared_ptr<const fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>>
    get_config(std::string, std::string) noexcept override {
        co_return fiber::tests::make_config_data(fiber::nacos::ConfigState::NotFound);
    }

    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    publish(std::string, std::string, std::string, fiber::nacos::ConfigType,
            std::optional<std::string>) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }

    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    remove_config(std::string, std::string) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }

    std::expected<fiber::nacos::Subscription<fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view,
              fiber::nacos::Subscription<fiber::nacos::ConfigData>::NotifyCallback on_notify, void *context) override {
        events_->add(std::string(data_id) + ".start");
        if (data_id == failed_subscription_) {
            return std::unexpected(fiber::nacos::ConfigServiceError{
                    .code = fiber::nacos::ConfigServiceErrorCode::Transport,
                    .io_error = fiber::common::IoErr::NotConnected,
                    .message = "injected subscription failure",
            });
        }

        auto *node = new SubscriptionNode{
                .events = events_,
                .stop_event = std::string(data_id) + ".stop",
        };
        if (data_id == "projects") {
            if (project_list_replay_ == ProjectListReplay::Closed) {
                on_notify(context, fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>{
                                           .kind = fiber::nacos::ResultKind::Closed,
                                   });
            } else {
                on_notify(context, fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>{
                                           .kind = fiber::nacos::ResultKind::Success,
                                           .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::NotFound),
                                   });
            }
        }
        return fiber::nacos::Subscription<fiber::nacos::ConfigData>(node, &close_subscription, &subscription_closed);
    }

private:
    struct SubscriptionNode {
        LifecycleEvents *events = nullptr;
        std::string stop_event;
        bool closed = false;
    };

    static void close_subscription(void *context) noexcept {
        std::unique_ptr<SubscriptionNode> node(static_cast<SubscriptionNode *>(context));
        node->closed = true;
        node->events->add(std::move(node->stop_event));
    }

    static bool subscription_closed(const void *context) noexcept {
        return static_cast<const SubscriptionNode *>(context)->closed;
    }

    LifecycleEvents *events_ = nullptr;
    bool fail_start_ = false;
    std::string failed_subscription_;
    ProjectListReplay project_list_replay_ = ProjectListReplay::NotFound;
};

class FakeNamingService final : public fiber::nacos::NamingService {
public:
    FakeNamingService(LifecycleEvents &events, bool fail_start) : events_(&events), fail_start_(fail_start) {}

    fiber::common::IoResult<void> start() noexcept override {
        events_->add("naming.start");
        if (fail_start_) {
            return std::unexpected(fiber::common::IoErr::NotConnected);
        }
        return {};
    }

    fiber::async::Task<void> shutdown() noexcept override {
        events_->add("naming.stop");
        co_return;
    }

    fiber::async::Task<
            std::expected<std::shared_ptr<const fiber::nacos::ServiceInfo>, fiber::nacos::NamingServiceError>>
    get(std::string, std::string) noexcept override {
        co_return std::unexpected(fiber::nacos::NamingServiceError{
                .code = fiber::nacos::NamingServiceErrorCode::Shutdown,
                .io_error = fiber::common::IoErr::Canceled,
        });
    }

    std::expected<fiber::nacos::Subscription<fiber::nacos::ServiceInfo>, fiber::nacos::NamingServiceError>
    subscribe(std::string_view, std::string_view, fiber::nacos::Subscription<fiber::nacos::ServiceInfo>::NotifyCallback,
              void *) override {
        return std::unexpected(fiber::nacos::NamingServiceError{
                .code = fiber::nacos::NamingServiceErrorCode::Shutdown,
                .io_error = fiber::common::IoErr::Canceled,
        });
    }

    std::expected<fiber::nacos::InstanceRegistration, fiber::nacos::NamingServiceError>
    registry(std::string_view, std::string_view, fiber::nacos::Instance) override {
        return std::unexpected(fiber::nacos::NamingServiceError{
                .code = fiber::nacos::NamingServiceErrorCode::Shutdown,
                .io_error = fiber::common::IoErr::Canceled,
        });
    }

private:
    LifecycleEvents *events_ = nullptr;
    bool fail_start_ = false;
};

enum class FailurePoint : std::uint8_t {
    None,
    CatClient,
    NacosClient,
    ConfigService,
    NamingService,
    GrayWatcher,
    TlsWatcher,
    AccessWatcher,
    InitialReadiness,
};

struct FailureCase {
    FailurePoint point = FailurePoint::None;
    std::optional<AccessServerRuntimeErrorCode> error;
    std::vector<std::string> expected_events;
};

class AccessControlPlaneSupervisorFailureTest : public testing::TestWithParam<FailureCase> {};

TEST(AccessControlPlaneSupervisorTest, KeepsResourceLifecycleAsSmallNonPolymorphicValue) {
    EXPECT_TRUE(std::is_trivially_copyable_v<AccessControlResourceLifecycle>);
    EXPECT_LE(sizeof(AccessControlResourceLifecycle), sizeof(void *) * 3U);
}

TEST_P(AccessControlPlaneSupervisorFailureTest, RollsBackOnlyAttemptedResourcesInStrictReverseOrder) {
    const FailureCase &test = GetParam();
    LifecycleEvents events;
    FakeResourceLifecycle cat{
            .events = &events,
            .name = "cat",
            .fail_start = test.point == FailurePoint::CatClient,
    };
    FakeResourceLifecycle nacos{
            .events = &events,
            .name = "nacos",
            .fail_start = test.point == FailurePoint::NacosClient,
    };
    auto config_service = std::make_unique<FakeConfigService>(
            events, test.point == FailurePoint::ConfigService,
            test.point == FailurePoint::GrayWatcher     ? "gray"
            : test.point == FailurePoint::TlsWatcher    ? "tls"
            : test.point == FailurePoint::AccessWatcher ? "projects"
                                                        : "",
            test.point == FailurePoint::InitialReadiness ? ProjectListReplay::Closed : ProjectListReplay::NotFound);
    auto naming_service = std::make_unique<FakeNamingService>(events, test.point == FailurePoint::NamingService);

    fiber::event::EventLoop coordinator_loop;
    fiber::event::EventLoopGroup nacos_group(1);
    fiber::event::EventLoopGroup compiler_group(1);
    fiber::event::EventLoopGroup cat_group(1);
    fiber::event::EventLoopGroup http_workers(1);
    nacos_group.start();
    compiler_group.start();
    cat_group.start();
    http_workers.start();

    bool completed = false;
    {
        AccessControlPlaneOptions options;
        options.config_watcher.project_list_data_id = "projects";
        options.config_watcher.project_route_data_id_prefix = "routes.";
        options.config_watcher.project_route_group = "routes-group";
        options.gray_watcher.data_id = "gray";
        options.gray_watcher.group = "gray-group";
        options.tls_certificate_watcher.data_id = "tls";
        options.tls_certificate_watcher.group = "tls-group";
        options.tls_enabled = test.point == FailurePoint::TlsWatcher || test.point == FailurePoint::AccessWatcher;

        AccessControlPlaneSupervisor supervisor(coordinator_loop, nacos_group.at(0), compiler_group.at(0),
                                                cat_group.at(0), http_workers, std::move(options),
                                                AccessControlPlaneDependencies{
                                                        .config_service = std::move(config_service),
                                                        .naming_service = std::move(naming_service),
                                                        .cat_lifecycle = cat.adapter(),
                                                        .nacos_lifecycle = nacos.adapter(),
                                                });

        fiber::async::spawn(coordinator_loop, [&]() -> fiber::async::DetachedTask {
            auto started = co_await supervisor.start();
            EXPECT_EQ(started.has_value(), !test.error.has_value());
            if (!started && test.error) {
                EXPECT_EQ(started.error().code, *test.error);
            }
            co_await supervisor.shutdown();
            const std::vector<std::string> after_first_shutdown = events.snapshot();
            co_await supervisor.shutdown();
            EXPECT_EQ(events.snapshot(), after_first_shutdown);
            completed = true;
            coordinator_loop.stop();
        });

        coordinator_loop.run();
    }

    http_workers.stop();
    http_workers.join();
    cat_group.stop();
    cat_group.join();
    compiler_group.stop();
    compiler_group.join();
    nacos_group.stop();
    nacos_group.join();

    EXPECT_TRUE(completed);
    EXPECT_EQ(events.snapshot(), test.expected_events);
}

std::string failure_case_name(const testing::TestParamInfo<FailureCase> &info) {
    switch (info.param.point) {
        case FailurePoint::None:
            return "Success";
        case FailurePoint::CatClient:
            return "CatClient";
        case FailurePoint::NacosClient:
            return "NacosClient";
        case FailurePoint::ConfigService:
            return "ConfigService";
        case FailurePoint::NamingService:
            return "NamingService";
        case FailurePoint::GrayWatcher:
            return "GrayWatcher";
        case FailurePoint::TlsWatcher:
            return "TlsWatcher";
        case FailurePoint::AccessWatcher:
            return "AccessWatcher";
        case FailurePoint::InitialReadiness:
            return "InitialReadiness";
    }
    return "Unknown";
}

INSTANTIATE_TEST_SUITE_P(
        StartupStages, AccessControlPlaneSupervisorFailureTest,
        testing::Values(
                FailureCase{
                        .expected_events = {"cat.start", "nacos.start", "config.start", "naming.start", "gray.start",
                                            "projects.start", "projects.stop", "gray.stop", "naming.stop",
                                            "config.stop", "nacos.stop", "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::CatClient,
                        .error = AccessServerRuntimeErrorCode::StartCatClient,
                        .expected_events = {"cat.start", "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::NacosClient,
                        .error = AccessServerRuntimeErrorCode::StartNacosClient,
                        .expected_events = {"cat.start", "nacos.start", "nacos.stop", "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::ConfigService,
                        .error = AccessServerRuntimeErrorCode::StartConfigService,
                        .expected_events = {"cat.start", "nacos.start", "config.start", "config.stop", "nacos.stop",
                                            "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::NamingService,
                        .error = AccessServerRuntimeErrorCode::StartNamingService,
                        .expected_events = {"cat.start", "nacos.start", "config.start", "naming.start", "naming.stop",
                                            "config.stop", "nacos.stop", "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::GrayWatcher,
                        .error = AccessServerRuntimeErrorCode::StartGrayWatcher,
                        .expected_events = {"cat.start", "nacos.start", "config.start", "naming.start", "gray.start",
                                            "naming.stop", "config.stop", "nacos.stop", "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::TlsWatcher,
                        .error = AccessServerRuntimeErrorCode::StartTlsCertificateWatcher,
                        .expected_events = {"cat.start", "nacos.start", "config.start", "naming.start", "gray.start",
                                            "tls.start", "gray.stop", "naming.stop", "config.stop", "nacos.stop",
                                            "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::AccessWatcher,
                        .error = AccessServerRuntimeErrorCode::StartAccessWatcher,
                        .expected_events = {"cat.start", "nacos.start", "config.start", "naming.start", "gray.start",
                                            "tls.start", "projects.start", "tls.stop", "gray.stop", "naming.stop",
                                            "config.stop", "nacos.stop", "cat.stop"},
                },
                FailureCase{
                        .point = FailurePoint::InitialReadiness,
                        .error = AccessServerRuntimeErrorCode::InitialConfigUnavailable,
                        .expected_events = {"cat.start", "nacos.start", "config.start", "naming.start", "gray.start",
                                            "projects.start", "projects.stop", "gray.stop", "naming.stop",
                                            "config.stop", "nacos.stop", "cat.stop"},
                }),
        failure_case_name);

} // namespace
