#include "AccessConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

std::string_view watcher_state_name(AccessConfigWatcherState state) noexcept {
    switch (state) {
        case AccessConfigWatcherState::Created:
            return "created";
        case AccessConfigWatcherState::Running:
            return "running";
        case AccessConfigWatcherState::Stopping:
            return "stopping";
        case AccessConfigWatcherState::Stopped:
            return "stopped";
    }
    return "created";
}

std::string_view readiness_state_name(AccessConfigReadinessState state) noexcept {
    switch (state) {
        case AccessConfigReadinessState::WaitingForProjectList:
            return "waiting_for_project_list";
        case AccessConfigReadinessState::SynchronizingProjects:
            return "synchronizing_projects";
        case AccessConfigReadinessState::Ready:
            return "ready";
        case AccessConfigReadinessState::Unavailable:
            return "unavailable";
        case AccessConfigReadinessState::Stopped:
            return "stopped";
    }
    return "waiting_for_project_list";
}

std::string_view subscription_state_name(AccessProjectSubscriptionState state) noexcept {
    switch (state) {
        case AccessProjectSubscriptionState::Subscribing:
            return "subscribing";
        case AccessProjectSubscriptionState::Subscribed:
            return "subscribed";
        case AccessProjectSubscriptionState::Retrying:
            return "retrying";
        case AccessProjectSubscriptionState::Failed:
            return "failed";
        case AccessProjectSubscriptionState::Retiring:
            return "retiring";
    }
    return "subscribing";
}

AccessActivationCandidateStatus candidate_status(AccessProjectConfigState state) noexcept {
    switch (state) {
        case AccessProjectConfigState::AwaitingValue:
            return AccessActivationCandidateStatus::Awaiting;
        case AccessProjectConfigState::Processing:
            return AccessActivationCandidateStatus::Processing;
        case AccessProjectConfigState::ReadyToPublish:
            return AccessActivationCandidateStatus::ReadyToPublish;
        case AccessProjectConfigState::Accepted:
            return AccessActivationCandidateStatus::Accepted;
        case AccessProjectConfigState::Rejected:
            return AccessActivationCandidateStatus::Rejected;
    }
    return AccessActivationCandidateStatus::Awaiting;
}

std::string_view failure_stage_name(AccessConfigWatcherFailureStage stage) noexcept {
    switch (stage) {
        case AccessConfigWatcherFailureStage::Subscription:
            return "subscription";
        case AccessConfigWatcherFailureStage::Decode:
            return "decode";
        case AccessConfigWatcherFailureStage::Compile:
            return "compile";
        case AccessConfigWatcherFailureStage::ServiceReady:
            return "service_ready";
        case AccessConfigWatcherFailureStage::Publish:
            return "publish";
    }
    return "decode";
}

std::string_view config_error_code_name(AccessConfigErrorCode code) noexcept {
    switch (code) {
        case AccessConfigErrorCode::InvalidJson:
            return "invalid_json";
        case AccessConfigErrorCode::InvalidRoot:
            return "invalid_root";
        case AccessConfigErrorCode::InvalidField:
            return "invalid_field";
        case AccessConfigErrorCode::OutOfRange:
            return "out_of_range";
        case AccessConfigErrorCode::InvalidCombination:
            return "invalid_combination";
        case AccessConfigErrorCode::Conflict:
            return "conflict";
        case AccessConfigErrorCode::LimitExceeded:
            return "limit_exceeded";
        case AccessConfigErrorCode::MissingDependency:
            return "missing_dependency";
    }
    return "invalid_configuration";
}

AccessActivationFailure activation_failure(const AccessConfigWatcherFailure &failure) {
    std::string code;
    if (failure.io_error != common::IoErr::None) {
        code = "io_";
        code.append(common::io_err_name(failure.io_error));
    } else {
        code = config_error_code_name(failure.error.code);
    }
    return AccessActivationFailure{
            .stage = std::string(failure_stage_name(failure.stage)),
            .code = std::move(code),
            .field = failure.error.field,
            .offset = failure.error.offset,
            .observed_at_unix_millis = failure.observed_at_unix_millis,
    };
}

AccessProjectSubscriptionState project_subscription_state(SubscriptionLifecycleState state) noexcept {
    switch (state) {
        case SubscriptionLifecycleState::Created:
        case SubscriptionLifecycleState::Subscribing:
            return AccessProjectSubscriptionState::Subscribing;
        case SubscriptionLifecycleState::Subscribed:
            return AccessProjectSubscriptionState::Subscribed;
        case SubscriptionLifecycleState::Retrying:
            return AccessProjectSubscriptionState::Retrying;
        case SubscriptionLifecycleState::Failed:
            return AccessProjectSubscriptionState::Failed;
        case SubscriptionLifecycleState::Stopped:
            return AccessProjectSubscriptionState::Retiring;
    }
    return AccessProjectSubscriptionState::Subscribing;
}

} // namespace

struct AccessConfigWatcher::ProjectListEntry final : public common::NonCopyable, public common::NonMovable {
    explicit ProjectListEntry(AccessConfigWatcher &value_owner) :
        owner(&value_owner), subscription(*value_owner.loop_) {}

    AccessConfigWatcher *owner = nullptr;
    SubscriptionLifecycle subscription;
};

struct AccessConfigWatcher::InitialProjectUpdate final : public common::NonCopyable, public common::NonMovable {
    InitialProjectUpdate(ReadyProjectUpdate value_ready, std::uint64_t value_generation, std::string value_data_id,
                         std::string value_md5) :
        ready(std::move(value_ready)), generation(value_generation), data_id(std::move(value_data_id)),
        md5(std::move(value_md5)) {}

    ReadyProjectUpdate ready;
    std::uint64_t generation = 0;
    std::string data_id;
    std::string md5;
};

struct AccessConfigWatcher::ProjectEntry final : public common::NonCopyable, public common::NonMovable {
    ProjectEntry(AccessConfigWatcher &value_owner, std::string value_project) :
        owner(&value_owner), project(std::move(value_project)), subscription(*value_owner.loop_) {}

    AccessConfigWatcher *owner = nullptr;
    std::string project;
    SubscriptionLifecycle subscription;
    AccessProjectConfigState config_state = AccessProjectConfigState::AwaitingValue;
    std::optional<AccessConfigWatcherFailure> last_failure;
    std::string observed_md5;
    std::optional<std::int32_t> observed_version;
    std::string active_md5;
    std::optional<std::int32_t> active_version;
    std::uint64_t active_snapshot_generation = 0;
    std::int64_t observed_at_unix_millis = 0;
    std::int64_t active_at_unix_millis = 0;
    std::uint64_t published_generation = 0;
    std::shared_ptr<const nacos::ConfigData> pending_compile_data;
    std::shared_ptr<const nacos::ConfigData> retry_identity_data;
    std::unique_ptr<InitialProjectUpdate> initial_update;
    ProjectCompileJob *active_compile_job = nullptr;
    bool synchronized = false;
    bool compile_queued = false;
    bool pending_force_compile = false;
};

struct AccessConfigWatcher::ProjectCompileJob final : public common::NonCopyable, public common::NonMovable {
    AccessConfigWatcher *owner = nullptr;
    std::shared_ptr<ProjectEntry> entry;
    std::shared_ptr<const nacos::ConfigData> data;
    std::optional<std::int32_t> published_version;
    std::optional<CompiledProjectConfigResult> result;
    std::atomic<bool> canceled{false};
    std::chrono::nanoseconds compile_duration{};
    std::uint64_t generation = 0;
    bool force_compile = false;
    bool compile_observed = false;
    event::EventLoop::NotifyEntry compile_entry;
    event::EventLoop::NotifyEntry completion_entry;
};

AccessConfigWatcher::AccessConfigWatcher(event::EventLoop &loop, AccessConfigCompiler &compiler,
                                         nacos::ConfigService &config_service, RouteConfigStore &store,
                                         AccessConfigWatcherOptions options, RouteSnapshotObserver observer,
                                         AccessConfigMetricsObserver metrics_observer,
                                         AccessRouteActivationEvidenceObserver activation_observer) :
    loop_(&loop), compiler_(&compiler), config_service_(&config_service), store_(&store), options_(std::move(options)),
    observer_(observer), metrics_observer_(metrics_observer), activation_observer_(activation_observer) {
    FIBER_ASSERT(loop_ != &compiler_->loop());
    readiness_publisher_ = readiness_.acquire_publisher();
    FIBER_ASSERT(readiness_publisher_.has_value());
}

AccessConfigWatcher::~AccessConfigWatcher() noexcept {
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
    initial_batch_active_ = store_->pin()->projects().empty();
    project_list_ = std::make_unique<ProjectListEntry>(*this);
    auto subscribed = project_list_->subscription.subscribe(*config_service_, options_.project_list_data_id,
                                                            options_.project_route_group, &project_list_notify,
                                                            project_list_.get());
    if (!subscribed) {
        observe_metric_event(AccessConfigMetricEvent::ProjectListSubscriptionFailed);
        project_list_->subscription.reset_start_failure();
        project_list_.reset();
        initial_batch_active_ = false;
        state_ = AccessConfigWatcherState::Created;
        return std::unexpected(std::move(subscribed.error()));
    }
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
        project_list_->subscription.stop();
    }
    for (auto &[project, entry]: projects_) {
        (void) project;
        entry->initial_update.reset();
        entry->subscription.stop();
        cancel_project_compile(entry);
    }
    compile_queue_.clear();
    co_await background_tasks_.join();
    FIBER_ASSERT(active_compiler_jobs_ == 0);
    initial_batch_active_ = false;
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
        if (owner.state_ == AccessConfigWatcherState::Running) {
            entry.subscription.fail(nacos::ConfigServiceError{
                    .code = nacos::ConfigServiceErrorCode::Shutdown,
                    .io_error = common::IoErr::NotConnected,
                    .message = "project-list subscription closed before shutdown",
            });
            owner.set_unavailable(owner.options_.project_list_data_id, common::IoErr::NotConnected,
                                  "project-list subscription closed before shutdown");
        }
        return;
    }
    if (result.data && owner.state_ == AccessConfigWatcherState::Running) {
        (void) entry.subscription.observe_value();
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
        hold->initial_update.reset();
        owner.cancel_project_compile(hold);
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
    if (result.data && owner.state_ == AccessConfigWatcherState::Running &&
        (hold->subscription.state() == SubscriptionLifecycleState::Subscribing ||
         hold->subscription.state() == SubscriptionLifecycleState::Subscribed)) {
        owner.apply_project(hold, result.data);
    }
}

void AccessConfigWatcher::apply_project_list(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (!initial_project_list_received_) {
        initial_project_list_received_ = true;
    }
    project_list_observed_md5_ = std::string(data.md5);
    project_list_observed_at_unix_millis_ = access_activation_unix_millis(*loop_);
    ProjectListResult parsed = data.state == nacos::ConfigState::NotFound
                                       ? ProjectListResult(std::vector<std::string>{})
                                       : parse_project_list(data.content);
    if (!parsed) {
        project_list_candidate_status_ = AccessActivationCandidateStatus::Rejected;
        report_failure(nullptr, AccessConfigWatcherFailureStage::Decode, options_.project_list_data_id,
                       std::string(data.md5), common::IoErr::Invalid, std::move(parsed.error()));
        project_list_failure_ = last_failure_;
        publish_readiness();
        return;
    }
    project_list_failure_.reset();
    project_list_candidate_status_ = AccessActivationCandidateStatus::Accepted;
    reconcile_projects(std::move(*parsed));
    project_list_active_md5_ = project_list_observed_md5_;
    project_list_active_at_unix_millis_ = project_list_observed_at_unix_millis_;
    observe_metric_event(AccessConfigMetricEvent::ProjectListAccepted);
    publish_readiness();
}

void AccessConfigWatcher::apply_project(const std::shared_ptr<ProjectEntry> &entry,
                                        std::shared_ptr<const nacos::ConfigData> data) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(data);
    entry->retry_identity_data.reset();
    entry->initial_update.reset();
    (void) entry->subscription.observe_value();
    cancel_project_compile(entry);
    entry->observed_md5 = std::string(data->md5);
    entry->observed_version.reset();
    entry->observed_at_unix_millis = access_activation_unix_millis(*loop_);
    entry->last_failure.reset();
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
        if (found == projects_.end() || found->second.get() != entry.get() ||
            entry->subscription.state() == SubscriptionLifecycleState::Stopped || !entry->pending_compile_data) {
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
        job->generation = entry->subscription.generation();
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
        const auto started = std::chrono::steady_clock::now();
        CompiledProjectConfigResult result = owner.compiler_->compile_project(
                job->entry->project, job->data->content, job->published_version, job->force_compile);
        job->compile_duration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        job->compile_observed = true;
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
    if (job->compile_observed) {
        owner.observe_metric_duration(AccessConfigMetricStage::ProjectCompile, job->compile_duration);
    }

    const auto found = owner.projects_.find(entry->project);
    const bool current = owner.state_ == AccessConfigWatcherState::Running && found != owner.projects_.end() &&
                         found->second.get() == entry.get() && entry->subscription.is_current(job->generation);
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
        if (prepared.error().code == AccessConfigErrorCode::MissingDependency) {
            entry->retry_identity_data = job.data;
        } else {
            entry->retry_identity_data.reset();
        }
        report_failure(entry, AccessConfigWatcherFailureStage::Compile,
                       options_.project_route_data_id_prefix + entry->project, std::string(job.data->md5),
                       common::IoErr::Invalid, std::move(prepared.error()));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }
    entry->retry_identity_data.reset();
    apply_prepared_project(entry, std::move(*prepared), job.generation, entry->subscription.revision_version(),
                           options_.project_route_data_id_prefix + entry->project, std::string(job.data->md5));
}

void AccessConfigWatcher::apply_prepared_project(const std::shared_ptr<ProjectEntry> &entry,
                                                 PreparedProjectUpdate prepared, std::uint64_t generation,
                                                 std::uint64_t revision_version, std::string data_id, std::string md5) {
    const auto started = std::chrono::steady_clock::now();
    auto ready = std::move(prepared).try_ready();
    if (ready) {
        observe_metric_duration(
                AccessConfigMetricStage::ServiceReady,
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started));
        commit_ready_project(entry, std::move(*ready), generation, std::move(data_id), std::move(md5));
        return;
    }
    background_tasks_.add();
    async::spawn([this, entry, prepared = std::move(prepared), generation, revision_version,
                  data_id = std::move(data_id), md5 = std::move(md5), started]() mutable {
        return await_ready_project(std::move(entry), std::move(prepared), generation, revision_version,
                                   std::move(data_id), std::move(md5), started);
    });
}

void AccessConfigWatcher::commit_ready_project(const std::shared_ptr<ProjectEntry> &entry, ReadyProjectUpdate ready,
                                               std::uint64_t generation, std::string data_id, std::string md5) {
    if (initial_batch_active_) {
        entry->initial_update = std::make_unique<InitialProjectUpdate>(std::move(ready), generation, std::move(data_id),
                                                                       std::move(md5));
        entry->config_state = AccessProjectConfigState::ReadyToPublish;
        publish_readiness();
        commit_initial_batch_if_ready();
        return;
    }

    auto updated = store_->commit(std::move(ready));
    if (!updated) {
        report_failure(entry, AccessConfigWatcherFailureStage::Publish, std::move(data_id), std::move(md5),
                       common::IoErr::Invalid, std::move(updated.error()));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }
    observe_publication_timing(updated->global_build_duration, updated->publish_duration);
    if (updated->status == ConfigUpdateStatus::Published || updated->status == ConfigUpdateStatus::Unloaded) {
        ++successful_updates_;
        entry->published_generation = generation;
        entry->active_md5 = md5;
        entry->active_version = entry->observed_version;
        entry->active_snapshot_generation = snapshot_generation_ + 1U;
        entry->active_at_unix_millis = access_activation_unix_millis(*loop_);
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

void AccessConfigWatcher::commit_initial_batch_if_ready() {
    FIBER_ASSERT(loop_->in_loop());
    if (!initial_batch_active_ || defer_readiness_updates_ || state_ != AccessConfigWatcherState::Running) {
        return;
    }
    for (const auto &[project, entry]: projects_) {
        (void) project;
        if (entry->initial_update) {
            continue;
        }
        if (entry->synchronized && (entry->config_state == AccessProjectConfigState::Accepted ||
                                    entry->config_state == AccessProjectConfigState::Rejected)) {
            continue;
        }
        return;
    }

    struct PendingResult {
        std::shared_ptr<ProjectEntry> entry;
        std::uint64_t generation = 0;
        std::string data_id;
        std::string md5;
    };

    initial_batch_active_ = false;
    std::vector<ReadyProjectUpdate> ready;
    std::vector<PendingResult> pending;
    ready.reserve(projects_.size());
    pending.reserve(projects_.size());
    for (auto &[project, entry]: projects_) {
        (void) project;
        if (!entry->initial_update) {
            continue;
        }
        InitialProjectUpdate &staged = *entry->initial_update;
        pending.push_back(PendingResult{
                .entry = entry,
                .generation = staged.generation,
                .data_id = std::move(staged.data_id),
                .md5 = std::move(staged.md5),
        });
        ready.push_back(std::move(staged.ready));
        entry->initial_update.reset();
    }

    if (ready.empty()) {
        publish_readiness();
        return;
    }

    auto updated = store_->commit_batch(std::move(ready));
    defer_readiness_updates_ = true;
    if (!updated) {
        for (PendingResult &item: pending) {
            report_failure(item.entry, AccessConfigWatcherFailureStage::Publish, std::move(item.data_id),
                           std::move(item.md5), common::IoErr::Invalid, updated.error());
            item.entry->config_state = AccessProjectConfigState::Rejected;
            item.entry->synchronized = true;
        }
    } else {
        observe_publication_timing(updated->global_build_duration, updated->publish_duration, updated->published);
        FIBER_ASSERT(updated->projects.size() == pending.size());
        for (std::size_t index = 0; index < pending.size(); ++index) {
            PendingResult &item = pending[index];
            ConfigBatchProjectResult &project_result = updated->projects[index];
            FIBER_ASSERT(project_result.project == item.entry->project);
            if (!project_result.outcome) {
                report_failure(item.entry, AccessConfigWatcherFailureStage::Publish, std::move(item.data_id),
                               std::move(item.md5), common::IoErr::Invalid, project_result.outcome.error());
                item.entry->config_state = AccessProjectConfigState::Rejected;
                item.entry->synchronized = true;
                continue;
            }

            const ConfigUpdateStatus status = *project_result.outcome;
            if (status == ConfigUpdateStatus::Published || status == ConfigUpdateStatus::Unloaded) {
                ++successful_updates_;
                item.entry->published_generation = item.generation;
                item.entry->active_md5 = item.md5;
                item.entry->active_version = item.entry->observed_version;
                item.entry->active_snapshot_generation = snapshot_generation_ + 1U;
                item.entry->active_at_unix_millis = access_activation_unix_millis(*loop_);
            }
            switch (status) {
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
            item.entry->config_state = AccessProjectConfigState::Accepted;
            item.entry->synchronized = true;
        }
        if (updated->published) {
            publish_observer(updated->snapshot);
        }
    }
    defer_readiness_updates_ = false;
    publish_readiness();
}

async::DetachedTask AccessConfigWatcher::await_ready_project(std::shared_ptr<ProjectEntry> entry,
                                                             PreparedProjectUpdate prepared, std::uint64_t generation,
                                                             std::uint64_t revision_version, std::string data_id,
                                                             std::string md5,
                                                             std::chrono::steady_clock::time_point started) noexcept {
    auto revisions = entry->subscription.subscribe_revisions();
    auto ready_or_replaced =
            co_await async::when_any([&prepared]() { return std::move(prepared).wait_ready().select(); },
                                     [&revisions, revision_version]() { return revisions.next(revision_version); });

    if (ready_or_replaced.is<0>()) {
        observe_metric_duration(
                AccessConfigMetricStage::ServiceReady,
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started));
        auto ready = std::move(ready_or_replaced).get<0>();
        const auto found = projects_.find(entry->project);
        const bool current = state_ == AccessConfigWatcherState::Running && found != projects_.end() &&
                             found->second.get() == entry.get() && entry->subscription.is_current(generation);
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
                                                                    SubscriptionRetryPlan plan) noexcept {
    auto revisions = entry->subscription.subscribe_revisions();
    auto delay_or_canceled = co_await async::when_any(
            [delay = plan.delay]() { return async::sleep(delay); },
            [&revisions, revision_version = plan.revision_version]() { return revisions.next(revision_version); });

    if (delay_or_canceled.is<0>()) {
        std::move(delay_or_canceled).get<0>();
        const auto found = projects_.find(entry->project);
        const bool current = state_ == AccessConfigWatcherState::Running && found != projects_.end() &&
                             found->second.get() == entry.get() &&
                             entry->subscription.state() == SubscriptionLifecycleState::Retrying;
        if (current) {
            subscribe_project(entry);
        }
    } else {
        std::move(delay_or_canceled).get<1>();
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
    commit_initial_batch_if_ready();
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
    retiring->initial_update.reset();
    retiring->subscription.stop();
    cancel_project_compile(retiring);

    if (initial_batch_active_) {
        publish_readiness();
        return;
    }

    auto removed = store_->remove_project(project);
    FIBER_ASSERT(removed.has_value());
    observe_publication_timing(removed->global_build_duration, removed->publish_duration);
    ++successful_updates_;
    observe_metric_event(AccessConfigMetricEvent::ProjectRouteRemoved);
    publish_observer(removed->snapshot);
    publish_readiness();
}

void AccessConfigWatcher::subscribe_project(const std::shared_ptr<ProjectEntry> &entry) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    if (state_ != AccessConfigWatcherState::Running ||
        entry->subscription.state() == SubscriptionLifecycleState::Stopped) {
        return;
    }

    cancel_project_compile(entry);
    entry->config_state = AccessProjectConfigState::AwaitingValue;
    entry->synchronized = false;
    entry->observed_md5.clear();
    entry->observed_version.reset();
    entry->observed_at_unix_millis = 0;
    publish_readiness();

    std::string data_id = options_.project_route_data_id_prefix;
    data_id.append(entry->project);
    auto subscribed = entry->subscription.subscribe(*config_service_, data_id, options_.project_route_group,
                                                    &project_notify, entry.get());
    if (!subscribed) {
        handle_subscription_failure(entry, std::move(data_id), std::move(subscribed.error()));
        return;
    }

    publish_readiness();
}

void AccessConfigWatcher::handle_subscription_failure(const std::shared_ptr<ProjectEntry> &entry, std::string data_id,
                                                      nacos::ConfigServiceError error) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    const common::IoErr io_error = error.io_error;
    if (entry->subscription.state() != SubscriptionLifecycleState::Failed) {
        entry->subscription.fail(error);
    }
    std::string message =
            error.message.empty() ? "failed to subscribe to project route configuration" : std::move(error.message);
    report_failure(entry, AccessConfigWatcherFailureStage::Subscription, std::move(data_id), {}, io_error,
                   AccessConfigError{
                           .code = AccessConfigErrorCode::InvalidCombination,
                           .field = "subscription",
                           .message = std::move(message),
                   });

    entry->initial_update.reset();
    cancel_project_compile(entry);
    entry->synchronized = false;
    entry->config_state = AccessProjectConfigState::AwaitingValue;
    if (state_ != AccessConfigWatcherState::Running ||
        entry->subscription.state() == SubscriptionLifecycleState::Stopped) {
        publish_readiness();
        return;
    }

    auto retry = entry->subscription.schedule_retry(SubscriptionRetryPolicy{
            .initial_delay = options_.subscription_retry_initial_delay,
            .maximum_delay = options_.subscription_retry_max_delay,
    });
    if (!retry) {
        publish_readiness();
        return;
    }
    background_tasks_.add();
    async::spawn([this, entry, plan = *retry]() { return retry_project_subscription(entry, plan); });
    publish_readiness();
}

void AccessConfigWatcher::settle_project(const std::shared_ptr<ProjectEntry> &entry,
                                         AccessProjectConfigState state) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    FIBER_ASSERT(state == AccessProjectConfigState::Accepted || state == AccessProjectConfigState::Rejected);
    entry->config_state = state;
    if (state == AccessProjectConfigState::Accepted) {
        entry->retry_identity_data.reset();
    }
    entry->synchronized = true;
    publish_readiness();
    commit_initial_batch_if_ready();
}

void AccessConfigWatcher::retry_missing_tls_identities() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != AccessConfigWatcherState::Running) {
        return;
    }
    for (const auto &[project, entry]: projects_) {
        (void) project;
        if (!entry->retry_identity_data || entry->config_state != AccessProjectConfigState::Rejected ||
            !entry->last_failure || entry->last_failure->error.code != AccessConfigErrorCode::MissingDependency ||
            !entry->subscription.subscribed()) {
            continue;
        }
        entry->config_state = AccessProjectConfigState::Processing;
        entry->synchronized = false;
        entry->last_failure.reset();
        enqueue_project_compile(entry, entry->retry_identity_data, true);
    }
    publish_readiness();
}

std::size_t AccessConfigWatcher::active_project_subscription_count() const noexcept {
    FIBER_ASSERT(loop_->in_loop());
    std::size_t count = 0;
    for (const auto &[project, entry]: projects_) {
        (void) project;
        if (entry->subscription.subscribed()) {
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
            .subscription_state = project_subscription_state(entry.subscription.state()),
            .config_state = entry.config_state,
            .first_value_received = entry.subscription.first_value_received(),
            .synchronized = entry.synchronized,
            .retry_attempt = entry.subscription.retry_attempt(),
            .next_retry_at = entry.subscription.next_retry_at(),
            .observed_md5 = entry.observed_md5,
            .observed_version = entry.observed_version,
            .generation = entry.subscription.generation(),
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
        if (entry->subscription.subscribed()) {
            ++next.subscribed_projects;
        }
        if (entry->synchronized) {
            ++next.synchronized_projects;
        }
        if (entry->subscription.state() == SubscriptionLifecycleState::Retrying) {
            ++next.retrying_projects;
        }
        if (entry->config_state == AccessProjectConfigState::Processing) {
            ++next.processing_projects;
        }
        if (entry->config_state == AccessProjectConfigState::ReadyToPublish) {
            ++next.ready_to_publish_projects;
        }
        if (entry->config_state == AccessProjectConfigState::Rejected) {
            ++next.rejected_projects;
        }
    }

    if (next.state == AccessConfigReadinessState::SynchronizingProjects &&
        next.subscribed_projects == next.desired_projects && next.synchronized_projects == next.desired_projects) {
        next.state = AccessConfigReadinessState::Ready;
    }
    publish_activation_evidence(next);
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
                                               .ready_to_publish_projects = next.ready_to_publish_projects,
                                               .rejected_projects = next.rejected_projects,
                                       });
    }
    readiness_publisher_->publish(std::move(next));
}

void AccessConfigWatcher::publish_activation_evidence(const AccessConfigReadiness &readiness) const noexcept {
    if (!activation_observer_.on_update) {
        return;
    }

    AccessRouteActivationEvidence evidence;
    evidence.watcher_state = watcher_state_name(state_);
    evidence.readiness_state = readiness_state_name(readiness.state);
    evidence.project_list = AccessActivationResourceEvidence{
            .data_id = options_.project_list_data_id,
            .group = options_.project_route_group,
            .candidate_status = project_list_candidate_status_,
            .observed_md5 = project_list_observed_md5_,
            .active_md5 = project_list_active_md5_,
            .observed_at_unix_millis = project_list_observed_at_unix_millis_,
            .active_at_unix_millis = project_list_active_at_unix_millis_,
            .failure = project_list_failure_ ? std::optional(activation_failure(*project_list_failure_)) : std::nullopt,
    };
    evidence.snapshot_generation = snapshot_generation_;
    evidence.snapshot_published_at_unix_millis = snapshot_published_at_unix_millis_;
    evidence.projects.reserve(projects_.size());

    std::unordered_set<std::string_view> loaded;
    const std::shared_ptr<const AccessRouteSnapshot> snapshot = store_->pin();
    loaded.reserve(snapshot->projects().size());
    for (const std::shared_ptr<const ProjectRouteSnapshot> &project: snapshot->projects()) {
        loaded.insert(project->project());
    }
    for (const auto &[project, entry]: projects_) {
        evidence.projects.push_back(AccessActivationProjectEvidence{
                .name = project,
                .data_id = options_.project_route_data_id_prefix + project,
                .group = options_.project_route_group,
                .subscription_state =
                        std::string(subscription_state_name(project_subscription_state(entry->subscription.state()))),
                .candidate_status = candidate_status(entry->config_state),
                .observed_md5 = entry->observed_md5,
                .observed_version = entry->observed_version,
                .active_md5 = entry->active_md5,
                .active_version = entry->active_version,
                .active_snapshot_generation = entry->active_snapshot_generation,
                .active_loaded = loaded.contains(project),
                .observed_at_unix_millis = entry->observed_at_unix_millis,
                .active_at_unix_millis = entry->active_at_unix_millis,
                .failure = entry->last_failure ? std::optional(activation_failure(*entry->last_failure)) : std::nullopt,
        });
    }
    activation_observer_.on_update(activation_observer_.context, evidence);
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

void AccessConfigWatcher::observe_metric_duration(AccessConfigMetricStage stage,
                                                  std::chrono::nanoseconds duration) const noexcept {
    if (metrics_observer_.on_duration) {
        metrics_observer_.on_duration(metrics_observer_.context, stage, duration);
    }
}

void AccessConfigWatcher::observe_publication_timing(std::chrono::nanoseconds global_build,
                                                     std::chrono::nanoseconds publish, bool published) const noexcept {
    observe_metric_duration(AccessConfigMetricStage::GlobalBuild, global_build);
    if (published) {
        observe_metric_duration(AccessConfigMetricStage::Publish, publish);
    }
}

void AccessConfigWatcher::publish_observer(const std::shared_ptr<const AccessRouteSnapshot> &snapshot) noexcept {
    FIBER_ASSERT(snapshot_generation_ != std::numeric_limits<std::uint64_t>::max());
    ++snapshot_generation_;
    snapshot_published_at_unix_millis_ = access_activation_unix_millis(*loop_);
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
            .observed_at_unix_millis = access_activation_unix_millis(*loop_),
    };
    if (entry) {
        entry->last_failure = failure;
    }
    last_failure_ = std::move(failure);
}

} // namespace fiber::access_server
