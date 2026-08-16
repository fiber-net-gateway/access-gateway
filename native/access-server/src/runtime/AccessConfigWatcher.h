#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_WATCHER_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_WATCHER_H

#include "AccessConfigCompiler.h"
#include "RouteConfigStore.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

enum class AccessConfigWatcherState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct AccessConfigWatcherOptions {
    std::string project_list_data_id = std::string(kProjectListDataId);
    std::string project_route_data_id_prefix = std::string(kProjectRouteDataIdPrefix);
    std::string project_route_group = std::string(kProjectRouteGroup);
    std::chrono::milliseconds subscription_retry_initial_delay{100};
    std::chrono::milliseconds subscription_retry_max_delay{30000};
};

enum class AccessConfigWatcherFailureStage : std::uint8_t {
    Subscription,
    Decode,
    Compile,
    ServiceReady,
    Publish,
};

struct AccessConfigWatcherFailure {
    AccessConfigWatcherFailureStage stage = AccessConfigWatcherFailureStage::Decode;
    std::string data_id;
    std::string md5;
    common::IoErr io_error = common::IoErr::None;
    AccessConfigError error;
};

enum class AccessProjectSubscriptionState : std::uint8_t {
    Subscribing,
    Subscribed,
    Retrying,
    Failed,
    Retiring,
};

enum class AccessProjectConfigState : std::uint8_t {
    AwaitingValue,
    Processing,
    Accepted,
    Rejected,
};

enum class AccessConfigReadinessState : std::uint8_t {
    WaitingForProjectList,
    SynchronizingProjects,
    Ready,
    Unavailable,
    Stopped,
};

struct AccessConfigReadiness {
    // Ready means the current subscription graph has reached a terminal first result. Rejected projects are
    // counted separately and Ready is not evidence that their candidate was published or activated.
    AccessConfigReadinessState state = AccessConfigReadinessState::WaitingForProjectList;
    std::size_t desired_projects = 0;
    std::size_t subscribed_projects = 0;
    std::size_t synchronized_projects = 0;
    std::size_t retrying_projects = 0;
    std::size_t processing_projects = 0;
    std::size_t rejected_projects = 0;
    common::IoErr io_error = common::IoErr::None;
    std::string message;

    bool operator==(const AccessConfigReadiness &) const = default;
};

struct AccessProjectConfigStatus {
    AccessProjectSubscriptionState subscription_state = AccessProjectSubscriptionState::Subscribing;
    AccessProjectConfigState config_state = AccessProjectConfigState::AwaitingValue;
    bool first_value_received = false;
    bool synchronized = false;
    std::uint32_t retry_attempt = 0;
    std::chrono::steady_clock::time_point next_retry_at{};
    std::string observed_md5;
    std::optional<std::int32_t> observed_version;
    std::uint64_t generation = 0;
    std::uint64_t published_generation = 0;
    std::optional<AccessConfigWatcherFailure> last_failure;
};

struct RouteSnapshotObserver {
    using Function = void (*)(void *context, std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept;

    void *context = nullptr;
    Function on_update = nullptr;
};

// Owns the Java-compatible two-level Nacos subscription graph:
// project-list -> one route-config subscription per listed project.
//
// start(), shutdown(), and all subscription mutations are owner-loop-only.
// RouteConfigStore publishes immutable snapshots for request workers.
class AccessConfigWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    AccessConfigWatcher(event::EventLoop &loop, AccessConfigCompiler &compiler, nacos::ConfigService &config_service,
                        RouteConfigStore &store, AccessConfigWatcherOptions options = {},
                        RouteSnapshotObserver observer = {});
    ~AccessConfigWatcher();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Watch<AccessConfigReadiness>::Subscriber subscribe_readiness() {
        return readiness_.subscribe();
    }

    [[nodiscard]] AccessConfigWatcherState state() const noexcept { return state_; }
    [[nodiscard]] bool initial_project_list_received() const noexcept { return initial_project_list_received_; }
    [[nodiscard]] std::size_t project_subscription_count() const noexcept { return projects_.size(); }
    [[nodiscard]] std::size_t active_project_subscription_count() const noexcept;
    [[nodiscard]] std::optional<AccessProjectConfigStatus> project_status(std::string_view project) const;
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<AccessConfigWatcherFailure> &last_failure() const noexcept {
        return last_failure_;
    }

private:
    struct ProjectListEntry;
    struct ProjectEntry;
    struct ProjectCompileJob;

    static void project_list_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    static void project_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    static void run_project_compile(ProjectCompileJob *job) noexcept;
    static void complete_project_compile(ProjectCompileJob *job) noexcept;

    void apply_project_list(const nacos::ConfigData &data);
    void apply_project(const std::shared_ptr<ProjectEntry> &entry, std::shared_ptr<const nacos::ConfigData> data);
    void enqueue_project_compile(const std::shared_ptr<ProjectEntry> &entry,
                                 std::shared_ptr<const nacos::ConfigData> data, bool force_compile = false);
    void dispatch_project_compile();
    void cancel_project_compile(const std::shared_ptr<ProjectEntry> &entry) noexcept;
    void apply_compiled_project(ProjectCompileJob &job);
    void apply_prepared_project(const std::shared_ptr<ProjectEntry> &entry, PreparedProjectUpdate prepared,
                                std::uint64_t generation, std::uint64_t revision_version, std::string data_id,
                                std::string md5);
    void commit_ready_project(const std::shared_ptr<ProjectEntry> &entry, ReadyProjectUpdate ready,
                              std::uint64_t generation, std::string data_id, std::string md5);
    [[nodiscard]] async::DetachedTask await_ready_project(std::shared_ptr<ProjectEntry> entry,
                                                          PreparedProjectUpdate prepared, std::uint64_t generation,
                                                          std::uint64_t revision_version, std::string data_id,
                                                          std::string md5) noexcept;
    [[nodiscard]] async::DetachedTask retry_project_subscription(std::shared_ptr<ProjectEntry> entry,
                                                                 std::uint64_t revision_version,
                                                                 std::chrono::milliseconds delay) noexcept;
    void reconcile_projects(std::vector<std::string> requested);
    void add_project(std::string project);
    void remove_project(std::string_view project);
    void subscribe_project(const std::shared_ptr<ProjectEntry> &entry);
    void handle_subscription_failure(const std::shared_ptr<ProjectEntry> &entry, std::string data_id,
                                     nacos::ConfigServiceError error);
    void settle_project(const std::shared_ptr<ProjectEntry> &entry, AccessProjectConfigState state) noexcept;
    void publish_readiness();
    void set_unavailable(std::string data_id, common::IoErr io_error, std::string message);
    void publish_observer(const std::shared_ptr<const AccessRouteSnapshot> &snapshot) const noexcept;
    void report_failure(const std::shared_ptr<ProjectEntry> &entry, AccessConfigWatcherFailureStage stage,
                        std::string data_id, std::string md5, common::IoErr io_error, AccessConfigError error);
    [[nodiscard]] std::chrono::milliseconds retry_delay(std::uint32_t attempt) const noexcept;
    [[nodiscard]] static bool retryable_subscription_error(nacos::ConfigServiceErrorCode code) noexcept;

    event::EventLoop *loop_ = nullptr;
    AccessConfigCompiler *compiler_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    RouteConfigStore *store_ = nullptr;
    AccessConfigWatcherOptions options_;
    RouteSnapshotObserver observer_;
    std::unique_ptr<ProjectListEntry> project_list_;
    std::map<std::string, std::shared_ptr<ProjectEntry>, std::less<>> projects_;
    std::deque<std::shared_ptr<ProjectEntry>> compile_queue_;
    std::optional<AccessConfigWatcherFailure> last_failure_;
    std::optional<AccessConfigWatcherFailure> unavailable_failure_;
    std::optional<AccessConfigWatcherFailure> project_list_failure_;
    async::Watch<AccessConfigReadiness> readiness_{AccessConfigReadiness{}};
    std::optional<async::Watch<AccessConfigReadiness>::Publisher> readiness_publisher_;
    AccessConfigReadiness published_readiness_;
    async::WaitGroup background_tasks_;
    AccessConfigWatcherState state_ = AccessConfigWatcherState::Created;
    bool initial_project_list_received_ = false;
    bool defer_readiness_updates_ = false;
    std::size_t active_compiler_jobs_ = 0;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_WATCHER_H
