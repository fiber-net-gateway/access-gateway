#include "AccessConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <set>
#include <utility>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

template<typename Entry>
void request_stop(Entry &entry) noexcept {
    if (!entry.stopping) {
        entry.stopping = true;
        entry.subscription.close();
    }
}

} // namespace

struct AccessConfigWatcher::ProjectListEntry final : public common::NonCopyable, public common::NonMovable {
    explicit ProjectListEntry(AccessConfigWatcher &value_owner) : owner(&value_owner) {}

    AccessConfigWatcher *owner = nullptr;
    nacos::Subscription<nacos::ConfigData> subscription;
    bool stopping = false;
};

struct AccessConfigWatcher::ProjectEntry final : public common::NonCopyable, public common::NonMovable {
    ProjectEntry(AccessConfigWatcher &value_owner, std::string value_project) :
        owner(&value_owner), project(std::move(value_project)), revisions(0),
        revision_publisher(revisions.acquire_publisher()) {
        FIBER_ASSERT(revision_publisher.has_value());
    }

    void advance() noexcept {
        FIBER_ASSERT(generation != std::numeric_limits<std::uint64_t>::max());
        revision_publisher->publish(++generation);
    }

    AccessConfigWatcher *owner = nullptr;
    std::string project;
    nacos::Subscription<nacos::ConfigData> subscription;
    async::Watch<std::uint64_t> revisions;
    std::optional<async::Watch<std::uint64_t>::Publisher> revision_publisher;
    AccessProjectSubscriptionState subscription_state = AccessProjectSubscriptionState::Subscribing;
    AccessProjectConfigState config_state = AccessProjectConfigState::AwaitingValue;
    std::optional<AccessConfigWatcherFailure> last_failure;
    std::string observed_md5;
    std::optional<std::int32_t> observed_version;
    std::chrono::steady_clock::time_point next_retry_at{};
    std::uint64_t generation = 0;
    std::uint64_t published_generation = 0;
    std::uint32_t retry_attempt = 0;
    std::shared_ptr<const nacos::ConfigData> pending_compile_data;
    ProjectCompileJob *active_compile_job = nullptr;
    bool first_value_received = false;
    bool synchronized = false;
    bool retry_scheduled = false;
    bool compile_queued = false;
    bool pending_force_compile = false;
    bool stopping = false;
};

struct AccessConfigWatcher::ProjectCompileJob final : public common::NonCopyable, public common::NonMovable {
    AccessConfigWatcher *owner = nullptr;
    std::shared_ptr<ProjectEntry> entry;
    std::shared_ptr<const nacos::ConfigData> data;
    std::optional<std::int32_t> published_version;
    std::optional<CompiledProjectConfigResult> result;
    std::atomic<bool> canceled{false};
    std::uint64_t generation = 0;
    bool force_compile = false;
    event::EventLoop::NotifyEntry compile_entry;
    event::EventLoop::NotifyEntry completion_entry;
};

AccessConfigWatcher::AccessConfigWatcher(event::EventLoop &loop, AccessConfigCompiler &compiler,
                                         nacos::ConfigService &config_service, RouteConfigStore &store,
                                         AccessConfigWatcherOptions options, RouteSnapshotObserver observer,
                                         AccessConfigMetricsObserver metrics_observer) :
    loop_(&loop), compiler_(&compiler), config_service_(&config_service), store_(&store), options_(std::move(options)),
    observer_(observer), metrics_observer_(metrics_observer) {
    FIBER_ASSERT(loop_ != &compiler_->loop());
    readiness_publisher_ = readiness_.acquire_publisher();
    FIBER_ASSERT(readiness_publisher_.has_value());
}

AccessConfigWatcher::~AccessConfigWatcher() {
    FIBER_ASSERT(state_ == AccessConfigWatcherState::Created || state_ == AccessConfigWatcherState::Stopped);
    FIBER_ASSERT(project_list_ == nullptr);
    FIBER_ASSERT(projects_.empty());
    FIBER_ASSERT(compile_queue_.empty());
    FIBER_ASSERT(active_compiler_jobs_ == 0);
    FIBER_ASSERT(background_tasks_.empty());
}

std::expected<void, nacos::ConfigServiceError> AccessConfigWatcher::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != AccessConfigWatcherState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "access config watcher is already started",
        });
    }
    if (options_.subscription_retry_initial_delay < std::chrono::milliseconds::zero() ||
        options_.subscription_retry_max_delay < options_.subscription_retry_initial_delay) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Invalid,
                .message = "project subscription retry delays are invalid",
        });
    }

    state_ = AccessConfigWatcherState::Running;
    project_list_ = std::make_unique<ProjectListEntry>(*this);
    auto subscription = config_service_->subscribe(options_.project_list_data_id, options_.project_route_group,
                                                   &project_list_notify, project_list_.get());
    if (!subscription) {
        observe_metric_event(AccessConfigMetricEvent::ProjectListSubscriptionFailed);
        project_list_.reset();
        state_ = AccessConfigWatcherState::Created;
        return std::unexpected(std::move(subscription.error()));
    }
    project_list_->subscription = std::move(*subscription);
    publish_readiness();
    return {};
}

async::Task<void> AccessConfigWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == AccessConfigWatcherState::Stopped) {
        co_return;
    }
    if (state_ == AccessConfigWatcherState::Created) {
        state_ = AccessConfigWatcherState::Stopped;
        publish_readiness();
        co_return;
    }
    if (state_ == AccessConfigWatcherState::Running) {
        state_ = AccessConfigWatcherState::Stopping;
    }
    if (project_list_) {
        request_stop(*project_list_);
    }
    for (auto &[project, entry]: projects_) {
        (void) project;
        entry->advance();
        cancel_project_compile(entry);
        entry->subscription_state = AccessProjectSubscriptionState::Retiring;
        request_stop(*entry);
    }
    compile_queue_.clear();
    co_await background_tasks_.join();
    FIBER_ASSERT(active_compiler_jobs_ == 0);
    projects_.clear();
    project_list_.reset();
    state_ = AccessConfigWatcherState::Stopped;
    publish_readiness();
}

void AccessConfigWatcher::project_list_notify(void *context,
                                              const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &entry = *static_cast<ProjectListEntry *>(context);
    AccessConfigWatcher &owner = *entry.owner;
    if (result.kind == nacos::ResultKind::Closed) {
        request_stop(entry);
        if (owner.state_ == AccessConfigWatcherState::Running) {
            owner.set_unavailable(owner.options_.project_list_data_id, common::IoErr::NotConnected,
                                  "project-list subscription closed before shutdown");
        }
        return;
    }
    if (result.data && owner.state_ == AccessConfigWatcherState::Running) {
        owner.apply_project_list(*result.data);
    }
}

void AccessConfigWatcher::project_notify(void *context,
                                         const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto *entry = static_cast<ProjectEntry *>(context);
    AccessConfigWatcher &owner = *entry->owner;
    const auto found = owner.projects_.find(entry->project);
    if (found == owner.projects_.end() || found->second.get() != entry) {
        return;
    }
    std::shared_ptr<ProjectEntry> hold = found->second;
    if (result.kind == nacos::ResultKind::Closed) {
        hold->advance();
        owner.cancel_project_compile(hold);
        request_stop(*hold);
        hold->first_value_received = false;
        hold->synchronized = false;
        owner.handle_subscription_failure(
                hold, owner.options_.project_route_data_id_prefix + hold->project,
                nacos::ConfigServiceError{
                        .code = nacos::ConfigServiceErrorCode::Shutdown,
                        .io_error = common::IoErr::NotConnected,
                        .message = "project route subscription closed before watcher shutdown",
                });
        return;
    }
    if (result.data && !hold->stopping && owner.state_ == AccessConfigWatcherState::Running) {
        owner.apply_project(hold, result.data);
    }
}

void AccessConfigWatcher::apply_project_list(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (!initial_project_list_received_) {
        initial_project_list_received_ = true;
    }
    ProjectListResult parsed = data.state == nacos::ConfigState::NotFound
                                       ? ProjectListResult(std::vector<std::string>{})
                                       : parse_project_list(data.content);
    if (!parsed) {
        report_failure(nullptr, AccessConfigWatcherFailureStage::Decode, options_.project_list_data_id,
                       std::string(data.md5), common::IoErr::Invalid, std::move(parsed.error()));
        project_list_failure_ = last_failure_;
        publish_readiness();
        return;
    }
    project_list_failure_.reset();
    reconcile_projects(std::move(*parsed));
    observe_metric_event(AccessConfigMetricEvent::ProjectListAccepted);
    publish_readiness();
}

void AccessConfigWatcher::apply_project(const std::shared_ptr<ProjectEntry> &entry,
                                        std::shared_ptr<const nacos::ConfigData> data) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(data);
    entry->advance();
    cancel_project_compile(entry);
    entry->first_value_received = true;
    entry->observed_md5 = std::string(data->md5);
    entry->observed_version.reset();
    entry->config_state = AccessProjectConfigState::Processing;
    publish_readiness();

    if (data->state == nacos::ConfigState::NotFound || data->content.empty()) {
        auto ignored = store_->prepare(entry->project, std::nullopt);
        FIBER_ASSERT(ignored.has_value());
        observe_metric_event(AccessConfigMetricEvent::ProjectRouteIgnoredEmpty);
        settle_project(entry, AccessProjectConfigState::Accepted);
        return;
    }
    if (data->content.size() > kAccessConfigLimits.project_route.max_payload_bytes) {
        report_failure(entry, AccessConfigWatcherFailureStage::Decode,
                       options_.project_route_data_id_prefix + entry->project, std::string(data->md5),
                       common::IoErr::Invalid,
                       AccessConfigError{
                               .code = AccessConfigErrorCode::LimitExceeded,
                               .field = "payload",
                               .message = "project route payload exceeds the configured byte limit",
                       });
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }
    enqueue_project_compile(entry, std::move(data));
}

void AccessConfigWatcher::enqueue_project_compile(const std::shared_ptr<ProjectEntry> &entry,
                                                  std::shared_ptr<const nacos::ConfigData> data, bool force_compile) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    FIBER_ASSERT(data);
    entry->pending_compile_data = std::move(data);
    entry->pending_force_compile = force_compile;
    if (entry->active_compile_job) {
        entry->active_compile_job->canceled.store(true, std::memory_order_release);
        return;
    }
    if (!entry->compile_queued) {
        entry->compile_queued = true;
        compile_queue_.push_back(entry);
    }
    dispatch_project_compile();
}

void AccessConfigWatcher::dispatch_project_compile() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != AccessConfigWatcherState::Running || active_compiler_jobs_ != 0) {
        return;
    }
    while (!compile_queue_.empty()) {
        std::shared_ptr<ProjectEntry> entry = std::move(compile_queue_.front());
        compile_queue_.pop_front();
        entry->compile_queued = false;
        const auto found = projects_.find(entry->project);
        if (found == projects_.end() || found->second.get() != entry.get() || entry->stopping ||
            !entry->pending_compile_data) {
            entry->pending_compile_data.reset();
            entry->pending_force_compile = false;
            continue;
        }

        auto data = std::exchange(entry->pending_compile_data, {});
        const bool force_compile = std::exchange(entry->pending_force_compile, false);
        auto *job = new (std::nothrow) ProjectCompileJob();
        if (!job) {
            report_failure(entry, AccessConfigWatcherFailureStage::Compile,
                           options_.project_route_data_id_prefix + entry->project, std::string(data->md5),
                           common::IoErr::NoMem,
                           AccessConfigError{
                                   .code = AccessConfigErrorCode::InvalidCombination,
                                   .field = "compiler",
                                   .message = "failed to allocate project compilation job",
                           });
            settle_project(entry, AccessProjectConfigState::Rejected);
            continue;
        }
        job->owner = this;
        job->entry = entry;
        job->data = std::move(data);
        job->published_version = store_->current_version(entry->project);
        job->generation = entry->generation;
        job->force_compile = force_compile;
        entry->active_compile_job = job;
        ++active_compiler_jobs_;
        background_tasks_.add();
        compiler_->loop().post<ProjectCompileJob, &ProjectCompileJob::compile_entry, &run_project_compile>(*job);
        return;
    }
}

void AccessConfigWatcher::run_project_compile(ProjectCompileJob *job) noexcept {
    FIBER_ASSERT(job);
    AccessConfigWatcher &owner = *job->owner;
    FIBER_ASSERT(owner.compiler_->loop().in_loop());
    if (!job->canceled.load(std::memory_order_acquire)) {
        CompiledProjectConfigResult result = owner.compiler_->compile_project(
                job->entry->project, job->data->content, job->published_version, job->force_compile);
        if (!job->canceled.load(std::memory_order_acquire)) {
            job->result.emplace(std::move(result));
        }
    }
    owner.loop_->post<ProjectCompileJob, &ProjectCompileJob::completion_entry, &complete_project_compile>(*job);
}

void AccessConfigWatcher::complete_project_compile(ProjectCompileJob *job) noexcept {
    FIBER_ASSERT(job);
    std::unique_ptr<ProjectCompileJob> owned(job);
    AccessConfigWatcher &owner = *job->owner;
    FIBER_ASSERT(owner.loop_->in_loop());
    std::shared_ptr<ProjectEntry> entry = job->entry;
    FIBER_ASSERT(entry->active_compile_job == job);
    entry->active_compile_job = nullptr;
    FIBER_ASSERT(owner.active_compiler_jobs_ == 1);
    --owner.active_compiler_jobs_;

    const auto found = owner.projects_.find(entry->project);
    const bool current = owner.state_ == AccessConfigWatcherState::Running && found != owner.projects_.end() &&
                         found->second.get() == entry.get() && entry->generation == job->generation;
    if (current && !job->canceled.load(std::memory_order_acquire) && job->result) {
        owner.apply_compiled_project(*job);
    }
    if (owner.state_ == AccessConfigWatcherState::Running && entry->pending_compile_data && !entry->compile_queued &&
        found != owner.projects_.end() && found->second.get() == entry.get()) {
        entry->compile_queued = true;
        owner.compile_queue_.push_back(entry);
    }
    owner.background_tasks_.done();
    owner.dispatch_project_compile();
}

void AccessConfigWatcher::cancel_project_compile(const std::shared_ptr<ProjectEntry> &entry) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (entry->active_compile_job) {
        entry->active_compile_job->canceled.store(true, std::memory_order_release);
    }
    entry->pending_compile_data.reset();
    entry->pending_force_compile = false;
    if (!entry->compile_queued) {
        return;
    }
    compile_queue_.erase(std::remove_if(compile_queue_.begin(), compile_queue_.end(),
                                        [&](const auto &candidate) { return candidate.get() == entry.get(); }),
                         compile_queue_.end());
    entry->compile_queued = false;
}

void AccessConfigWatcher::apply_compiled_project(ProjectCompileJob &job) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(job.result);
    std::shared_ptr<ProjectEntry> entry = job.entry;
    CompiledProjectConfigResult result = std::move(*job.result);
    if (!result) {
        entry->observed_version = result.error().observed_version;
        const AccessConfigWatcherFailureStage stage = result.error().stage == AccessProjectCompileFailureStage::Decode
                                                              ? AccessConfigWatcherFailureStage::Decode
                                                              : AccessConfigWatcherFailureStage::Compile;
        report_failure(entry, stage, options_.project_route_data_id_prefix + entry->project, std::string(job.data->md5),
                       common::IoErr::Invalid, std::move(result.error().error));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }

    entry->observed_version = result->version;
    if (result->compilation_skipped) {
        if (result->version && store_->current_version(entry->project) == result->version) {
            observe_metric_event(AccessConfigMetricEvent::ProjectRouteVersionUnchanged);
            settle_project(entry, AccessProjectConfigState::Accepted);
        } else {
            enqueue_project_compile(entry, job.data, true);
        }
        return;
    }
    auto prepared = store_->prepare_compiled(entry->project, result->version, std::move(result->snapshot));
    if (!prepared) {
        report_failure(entry, AccessConfigWatcherFailureStage::Compile,
                       options_.project_route_data_id_prefix + entry->project, std::string(job.data->md5),
                       common::IoErr::Invalid, std::move(prepared.error()));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }
    apply_prepared_project(entry, std::move(*prepared), job.generation, entry->revisions.current().version,
                           options_.project_route_data_id_prefix + entry->project, std::string(job.data->md5));
}

void AccessConfigWatcher::apply_prepared_project(const std::shared_ptr<ProjectEntry> &entry,
                                                 PreparedProjectUpdate prepared, std::uint64_t generation,
                                                 std::uint64_t revision_version, std::string data_id, std::string md5) {
    auto ready = std::move(prepared).try_ready();
    if (ready) {
        commit_ready_project(entry, std::move(*ready), generation, std::move(data_id), std::move(md5));
        return;
    }
    background_tasks_.add();
    async::spawn([this, entry, prepared = std::move(prepared), generation, revision_version,
                  data_id = std::move(data_id), md5 = std::move(md5)]() mutable {
        return await_ready_project(std::move(entry), std::move(prepared), generation, revision_version,
                                   std::move(data_id), std::move(md5));
    });
}

void AccessConfigWatcher::commit_ready_project(const std::shared_ptr<ProjectEntry> &entry, ReadyProjectUpdate ready,
                                               std::uint64_t generation, std::string data_id, std::string md5) {
    auto updated = store_->commit(std::move(ready));
    if (!updated) {
        report_failure(entry, AccessConfigWatcherFailureStage::Publish, std::move(data_id), std::move(md5),
                       common::IoErr::Invalid, std::move(updated.error()));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }
    if (updated->status == ConfigUpdateStatus::Published || updated->status == ConfigUpdateStatus::Unloaded) {
        ++successful_updates_;
        entry->published_generation = generation;
        publish_observer(updated->snapshot);
    }
    switch (updated->status) {
        case ConfigUpdateStatus::IgnoredEmpty:
            observe_metric_event(AccessConfigMetricEvent::ProjectRouteIgnoredEmpty);
            break;
        case ConfigUpdateStatus::VersionUnchanged:
            observe_metric_event(AccessConfigMetricEvent::ProjectRouteVersionUnchanged);
            break;
        case ConfigUpdateStatus::Published:
            observe_metric_event(AccessConfigMetricEvent::ProjectRoutePublished);
            break;
        case ConfigUpdateStatus::Unloaded:
            observe_metric_event(AccessConfigMetricEvent::ProjectRouteUnloaded);
            break;
        case ConfigUpdateStatus::ProjectRemoved:
            FIBER_ASSERT(false);
            break;
    }
    settle_project(entry, AccessProjectConfigState::Accepted);
}

async::DetachedTask AccessConfigWatcher::await_ready_project(std::shared_ptr<ProjectEntry> entry,
                                                             PreparedProjectUpdate prepared, std::uint64_t generation,
                                                             std::uint64_t revision_version, std::string data_id,
                                                             std::string md5) noexcept {
    auto revisions = entry->revisions.subscribe();
    auto ready_or_replaced =
            co_await async::when_any([&prepared]() { return std::move(prepared).wait_ready().select(); },
                                     [&revisions, revision_version]() { return revisions.next(revision_version); });

    if (ready_or_replaced.is<0>()) {
        auto ready = std::move(ready_or_replaced).get<0>();
        const auto found = projects_.find(entry->project);
        const bool current = state_ == AccessConfigWatcherState::Running && found != projects_.end() &&
                             found->second.get() == entry.get() && entry->generation == generation;
        if (current && !ready) {
            const common::IoErr io_error = ready.error().io_error;
            report_failure(entry, AccessConfigWatcherFailureStage::ServiceReady, std::move(data_id), std::move(md5),
                           io_error,
                           AccessConfigError{
                                   .code = AccessConfigErrorCode::InvalidCombination,
                                   .field = "service",
                                   .message = ready.error().message,
                           });
            settle_project(entry, AccessProjectConfigState::Rejected);
        } else if (current) {
            commit_ready_project(entry, std::move(*ready), generation, std::move(data_id), std::move(md5));
        }
    } else {
        std::move(ready_or_replaced).get<1>();
    }
    background_tasks_.done();
}

async::DetachedTask AccessConfigWatcher::retry_project_subscription(std::shared_ptr<ProjectEntry> entry,
                                                                    std::uint64_t revision_version,
                                                                    std::chrono::milliseconds delay) noexcept {
    auto revisions = entry->revisions.subscribe();
    auto delay_or_canceled =
            co_await async::when_any([delay]() { return async::sleep(delay); },
                                     [&revisions, revision_version]() { return revisions.next(revision_version); });

    if (delay_or_canceled.is<0>()) {
        std::move(delay_or_canceled).get<0>();
        const auto found = projects_.find(entry->project);
        const bool current = state_ == AccessConfigWatcherState::Running && found != projects_.end() &&
                             found->second.get() == entry.get() && !entry->stopping && entry->retry_scheduled;
        entry->retry_scheduled = false;
        if (current) {
            subscribe_project(entry);
        }
    } else {
        std::move(delay_or_canceled).get<1>();
        entry->retry_scheduled = false;
    }
    background_tasks_.done();
}

void AccessConfigWatcher::reconcile_projects(std::vector<std::string> requested) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!defer_readiness_updates_);
    defer_readiness_updates_ = true;
    std::set<std::string, std::less<>> unique;
    for (std::string &project: requested) {
        if (unique.emplace(project).second && !projects_.contains(project)) {
            add_project(std::move(project));
        }
    }

    std::vector<std::string> removed;
    removed.reserve(projects_.size());
    for (const auto &[project, entry]: projects_) {
        (void) entry;
        if (!unique.contains(project)) {
            removed.push_back(project);
        }
    }
    for (const std::string &project: removed) {
        remove_project(project);
    }
    defer_readiness_updates_ = false;
}

void AccessConfigWatcher::add_project(std::string project) {
    FIBER_ASSERT(loop_->in_loop());
    auto entry = std::make_shared<ProjectEntry>(*this, std::move(project));
    auto [iterator, inserted] = projects_.emplace(entry->project, entry);
    (void) iterator;
    FIBER_ASSERT(inserted);
    subscribe_project(entry);
}

void AccessConfigWatcher::remove_project(std::string_view project) {
    FIBER_ASSERT(loop_->in_loop());
    const auto iterator = projects_.find(project);
    if (iterator == projects_.end()) {
        return;
    }
    std::shared_ptr<ProjectEntry> retiring = std::move(iterator->second);
    projects_.erase(iterator);
    retiring->advance();
    cancel_project_compile(retiring);
    retiring->subscription_state = AccessProjectSubscriptionState::Retiring;
    request_stop(*retiring);

    auto removed = store_->remove_project(project);
    FIBER_ASSERT(removed.has_value());
    ++successful_updates_;
    observe_metric_event(AccessConfigMetricEvent::ProjectRouteRemoved);
    publish_observer(removed->snapshot);
    publish_readiness();
}

void AccessConfigWatcher::subscribe_project(const std::shared_ptr<ProjectEntry> &entry) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    if (state_ != AccessConfigWatcherState::Running || entry->stopping) {
        return;
    }

    cancel_project_compile(entry);
    entry->subscription.close();
    entry->subscription_state = AccessProjectSubscriptionState::Subscribing;
    entry->config_state = AccessProjectConfigState::AwaitingValue;
    entry->first_value_received = false;
    entry->synchronized = false;
    entry->observed_md5.clear();
    entry->observed_version.reset();
    entry->next_retry_at = {};
    publish_readiness();

    std::string data_id = options_.project_route_data_id_prefix;
    data_id.append(entry->project);
    auto subscription = config_service_->subscribe(data_id, options_.project_route_group, &project_notify, entry.get());
    if (!subscription) {
        handle_subscription_failure(entry, std::move(data_id), std::move(subscription.error()));
        return;
    }

    entry->subscription = std::move(*subscription);
    entry->subscription_state = AccessProjectSubscriptionState::Subscribed;
    entry->retry_attempt = 0;
    entry->next_retry_at = {};
    publish_readiness();
}

void AccessConfigWatcher::handle_subscription_failure(const std::shared_ptr<ProjectEntry> &entry, std::string data_id,
                                                      nacos::ConfigServiceError error) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    const nacos::ConfigServiceErrorCode code = error.code;
    const common::IoErr io_error = error.io_error;
    std::string message =
            error.message.empty() ? "failed to subscribe to project route configuration" : std::move(error.message);
    report_failure(entry, AccessConfigWatcherFailureStage::Subscription, std::move(data_id), {}, io_error,
                   AccessConfigError{
                           .code = AccessConfigErrorCode::InvalidCombination,
                           .field = "subscription",
                           .message = std::move(message),
                   });

    cancel_project_compile(entry);
    entry->subscription.close();
    entry->synchronized = false;
    entry->first_value_received = false;
    entry->config_state = AccessProjectConfigState::AwaitingValue;
    entry->next_retry_at = {};
    if (state_ != AccessConfigWatcherState::Running || entry->stopping || !retryable_subscription_error(code)) {
        entry->subscription_state = AccessProjectSubscriptionState::Failed;
        publish_readiness();
        return;
    }

    FIBER_ASSERT(!entry->retry_scheduled);
    FIBER_ASSERT(entry->retry_attempt != std::numeric_limits<std::uint32_t>::max());
    const std::chrono::milliseconds delay = retry_delay(++entry->retry_attempt);
    entry->subscription_state = AccessProjectSubscriptionState::Retrying;
    entry->next_retry_at = event::EventLoop::current().now() + delay;
    entry->retry_scheduled = true;
    const std::uint64_t revision_version = entry->revisions.current().version;
    background_tasks_.add();
    async::spawn([this, entry, revision_version, delay]() {
        return retry_project_subscription(entry, revision_version, delay);
    });
    publish_readiness();
}

void AccessConfigWatcher::settle_project(const std::shared_ptr<ProjectEntry> &entry,
                                         AccessProjectConfigState state) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    FIBER_ASSERT(state == AccessProjectConfigState::Accepted || state == AccessProjectConfigState::Rejected);
    entry->config_state = state;
    entry->synchronized = true;
    publish_readiness();
}

std::size_t AccessConfigWatcher::active_project_subscription_count() const noexcept {
    FIBER_ASSERT(loop_->in_loop());
    std::size_t count = 0;
    for (const auto &[project, entry]: projects_) {
        (void) project;
        if (entry->subscription_state == AccessProjectSubscriptionState::Subscribed && entry->subscription &&
            !entry->subscription.closed()) {
            ++count;
        }
    }
    return count;
}

std::optional<AccessProjectConfigStatus> AccessConfigWatcher::project_status(std::string_view project) const {
    FIBER_ASSERT(loop_->in_loop());
    const auto iterator = projects_.find(project);
    if (iterator == projects_.end()) {
        return std::nullopt;
    }
    const ProjectEntry &entry = *iterator->second;
    return AccessProjectConfigStatus{
            .subscription_state = entry.subscription_state,
            .config_state = entry.config_state,
            .first_value_received = entry.first_value_received,
            .synchronized = entry.synchronized,
            .retry_attempt = entry.retry_attempt,
            .next_retry_at = entry.next_retry_at,
            .observed_md5 = entry.observed_md5,
            .observed_version = entry.observed_version,
            .generation = entry.generation,
            .published_generation = entry.published_generation,
            .last_failure = entry.last_failure,
    };
}

void AccessConfigWatcher::publish_readiness() {
    FIBER_ASSERT(loop_->in_loop());
    if (defer_readiness_updates_) {
        return;
    }
    AccessConfigReadiness next;
    if (state_ == AccessConfigWatcherState::Stopped) {
        next.state = AccessConfigReadinessState::Stopped;
    } else if (unavailable_failure_) {
        next.state = AccessConfigReadinessState::Unavailable;
        next.io_error = unavailable_failure_->io_error;
        next.message = unavailable_failure_->error.message;
    } else if (project_list_failure_) {
        next.state = AccessConfigReadinessState::Unavailable;
        next.io_error = project_list_failure_->io_error;
        next.message = project_list_failure_->error.message;
    } else if (!initial_project_list_received_) {
        next.state = AccessConfigReadinessState::WaitingForProjectList;
    } else {
        next.state = AccessConfigReadinessState::SynchronizingProjects;
    }

    for (const auto &[project, entry]: projects_) {
        (void) project;
        ++next.desired_projects;
        if (entry->subscription_state == AccessProjectSubscriptionState::Subscribed && entry->subscription &&
            !entry->subscription.closed()) {
            ++next.subscribed_projects;
        }
        if (entry->synchronized) {
            ++next.synchronized_projects;
        }
        if (entry->subscription_state == AccessProjectSubscriptionState::Retrying) {
            ++next.retrying_projects;
        }
        if (entry->config_state == AccessProjectConfigState::Processing) {
            ++next.processing_projects;
        }
        if (entry->config_state == AccessProjectConfigState::Rejected) {
            ++next.rejected_projects;
        }
    }

    if (next.state == AccessConfigReadinessState::SynchronizingProjects &&
        next.subscribed_projects == next.desired_projects && next.synchronized_projects == next.desired_projects) {
        next.state = AccessConfigReadinessState::Ready;
    }
    if (next == published_readiness_) {
        return;
    }
    published_readiness_ = next;
    if (metrics_observer_.on_readiness) {
        AccessConfigMetricReadinessState state = AccessConfigMetricReadinessState::WaitingForProjectList;
        switch (next.state) {
            case AccessConfigReadinessState::WaitingForProjectList:
                state = AccessConfigMetricReadinessState::WaitingForProjectList;
                break;
            case AccessConfigReadinessState::SynchronizingProjects:
                state = AccessConfigMetricReadinessState::SynchronizingProjects;
                break;
            case AccessConfigReadinessState::Ready:
                state = AccessConfigMetricReadinessState::Ready;
                break;
            case AccessConfigReadinessState::Unavailable:
                state = AccessConfigMetricReadinessState::Unavailable;
                break;
            case AccessConfigReadinessState::Stopped:
                state = AccessConfigMetricReadinessState::Stopped;
                break;
        }
        metrics_observer_.on_readiness(metrics_observer_.context,
                                       AccessConfigMetricReadiness{
                                               .state = state,
                                               .desired_projects = next.desired_projects,
                                               .subscribed_projects = next.subscribed_projects,
                                               .synchronized_projects = next.synchronized_projects,
                                               .retrying_projects = next.retrying_projects,
                                               .processing_projects = next.processing_projects,
                                               .rejected_projects = next.rejected_projects,
                                       });
    }
    readiness_publisher_->publish(std::move(next));
}

void AccessConfigWatcher::set_unavailable(std::string data_id, common::IoErr io_error, std::string message) {
    FIBER_ASSERT(loop_->in_loop());
    report_failure(nullptr, AccessConfigWatcherFailureStage::Subscription, std::move(data_id), {}, io_error,
                   AccessConfigError{
                           .code = AccessConfigErrorCode::InvalidCombination,
                           .field = "subscription",
                           .message = std::move(message),
                   });
    unavailable_failure_ = last_failure_;
    publish_readiness();
}

void AccessConfigWatcher::observe_metric_event(AccessConfigMetricEvent event) const noexcept {
    if (metrics_observer_.on_event) {
        metrics_observer_.on_event(metrics_observer_.context, event);
    }
}

void AccessConfigWatcher::publish_observer(const std::shared_ptr<const AccessRouteSnapshot> &snapshot) const noexcept {
    if (observer_.on_update) {
        observer_.on_update(observer_.context, snapshot);
    }
    if (metrics_observer_.on_snapshot) {
        metrics_observer_.on_snapshot(metrics_observer_.context, *snapshot);
    }
}

void AccessConfigWatcher::report_failure(const std::shared_ptr<ProjectEntry> &entry,
                                         AccessConfigWatcherFailureStage stage, std::string data_id, std::string md5,
                                         common::IoErr io_error, AccessConfigError error) {
    ++failed_updates_;
    if (!entry) {
        FIBER_ASSERT(stage == AccessConfigWatcherFailureStage::Subscription ||
                     stage == AccessConfigWatcherFailureStage::Decode);
        observe_metric_event(stage == AccessConfigWatcherFailureStage::Decode
                                     ? AccessConfigMetricEvent::ProjectListDecodeFailed
                                     : AccessConfigMetricEvent::ProjectListSubscriptionFailed);
    } else {
        switch (stage) {
            case AccessConfigWatcherFailureStage::Subscription:
                observe_metric_event(AccessConfigMetricEvent::ProjectRouteSubscriptionFailed);
                break;
            case AccessConfigWatcherFailureStage::Decode:
                observe_metric_event(AccessConfigMetricEvent::ProjectRouteDecodeFailed);
                break;
            case AccessConfigWatcherFailureStage::Compile:
                observe_metric_event(AccessConfigMetricEvent::ProjectRouteCompileFailed);
                break;
            case AccessConfigWatcherFailureStage::ServiceReady:
                observe_metric_event(AccessConfigMetricEvent::ProjectRouteServiceReadyFailed);
                break;
            case AccessConfigWatcherFailureStage::Publish:
                observe_metric_event(AccessConfigMetricEvent::ProjectRoutePublishFailed);
                break;
        }
    }
    AccessConfigWatcherFailure failure{
            .stage = stage,
            .data_id = std::move(data_id),
            .md5 = std::move(md5),
            .io_error = io_error,
            .error = std::move(error),
    };
    if (entry) {
        entry->last_failure = failure;
    }
    last_failure_ = std::move(failure);
}

std::chrono::milliseconds AccessConfigWatcher::retry_delay(std::uint32_t attempt) const noexcept {
    std::chrono::milliseconds delay = options_.subscription_retry_initial_delay;
    const std::chrono::milliseconds maximum = options_.subscription_retry_max_delay;
    for (std::uint32_t current = 1; current < attempt && delay < maximum; ++current) {
        if (delay.count() > maximum.count() / 2) {
            return maximum;
        }
        delay *= 2;
    }
    return std::min(delay, maximum);
}

bool AccessConfigWatcher::retryable_subscription_error(nacos::ConfigServiceErrorCode code) noexcept {
    switch (code) {
        case nacos::ConfigServiceErrorCode::AuthenticationUnavailable:
        case nacos::ConfigServiceErrorCode::Transport:
        case nacos::ConfigServiceErrorCode::GrpcStatus:
        case nacos::ConfigServiceErrorCode::Protocol:
        case nacos::ConfigServiceErrorCode::Server:
            return true;
        case nacos::ConfigServiceErrorCode::InvalidArgument:
        case nacos::ConfigServiceErrorCode::Shutdown:
        case nacos::ConfigServiceErrorCode::ContentTooLarge:
            return false;
    }
    return false;
}

} // namespace fiber::access_server
