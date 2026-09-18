#include "AccessConfigWatcher.h"

#include "../config/AccessConfigCodec.h"
#include "../observability/AccessServerLogCategories.h"

#include <algorithm>
#include <chrono>
#include <limits>
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
#include <fiber/log/Log.h>

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_CONFIG, kAccessServerConfigLogger);

// Project compilation runs inline on the nacos loop. Past this threshold a
// compile is worth a warning: it delays subscription heartbeats and other
// loop work (Nacos evicts silent subscribers after ~15s).
constexpr std::chrono::milliseconds kProjectCompileWarnThreshold{50};

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
    std::shared_ptr<const nacos::ConfigData> retry_identity_data;
    std::unique_ptr<InitialProjectUpdate> initial_update;
    bool synchronized = false;
};

AccessConfigWatcher::AccessConfigWatcher(event::EventLoop &loop, AccessConfigCompiler &compiler,
                                         nacos::ConfigService &config_service, RouteConfigStore &store,
                                         AccessConfigWatcherOptions options, RouteSnapshotObserver observer,
                                         AccessConfigMetricsObserver metrics_observer,
                                         AccessRouteActivationEvidenceObserver activation_observer) :
    loop_(&loop), compiler_(&compiler), config_service_(&config_service), store_(&store), options_(std::move(options)),
    observer_(observer), metrics_observer_(metrics_observer), activation_observer_(activation_observer) {
    // Route candidates compile inline on this loop; the compiler must be bound
    // to it so its loop-ownership assertions hold.
    FIBER_ASSERT(loop_ == &compiler_->loop());
    readiness_publisher_ = readiness_.acquire_publisher();
    FIBER_ASSERT(readiness_publisher_.has_value());
}

AccessConfigWatcher::~AccessConfigWatcher() noexcept {
    FIBER_ASSERT(state_ == AccessConfigWatcherState::Created || state_ == AccessConfigWatcherState::Stopped);
    FIBER_ASSERT(project_list_ == nullptr);
    FIBER_ASSERT(projects_.empty());
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
    initial_snapshot_published_ = !initial_batch_active_;
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
    }
    co_await background_tasks_.join();
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
    if (data.state == nacos::ConfigState::NotFound && !initial_snapshot_published_) {
        project_list_candidate_status_ = AccessActivationCandidateStatus::Rejected;
        report_failure(nullptr, AccessConfigWatcherFailureStage::Decode, options_.project_list_data_id,
                       std::string(data.md5), common::IoErr::NotFound,
                       AccessConfigError{
                               .code = AccessConfigErrorCode::InvalidCombination,
                               .field = "project_list",
                               .message = "project list configuration was not found",
                       });
        project_list_failure_ = last_failure_;
        unavailable_failure_ = project_list_failure_;
        publish_readiness();
        return;
    }
    ProjectListResult parsed = data.state == nacos::ConfigState::NotFound
                                       ? ProjectListResult(std::vector<std::string>{})
                                       : parse_project_list(data.content);
    if (!parsed) {
        project_list_candidate_status_ = AccessActivationCandidateStatus::Rejected;
        report_failure(nullptr, AccessConfigWatcherFailureStage::Decode, options_.project_list_data_id,
                       std::string(data.md5), common::IoErr::Invalid, std::move(parsed.error()));
        project_list_failure_ = last_failure_;
        if (!initial_snapshot_published_) {
            unavailable_failure_ = project_list_failure_;
        }
        publish_readiness();
        return;
    }
    if (!initial_snapshot_published_ && parsed->empty()) {
        project_list_candidate_status_ = AccessActivationCandidateStatus::Rejected;
        report_failure(nullptr, AccessConfigWatcherFailureStage::Decode, options_.project_list_data_id,
                       std::string(data.md5), common::IoErr::Invalid,
                       AccessConfigError{
                               .code = AccessConfigErrorCode::InvalidCombination,
                               .field = "project_list",
                               .message = "initial project list configuration is empty",
                       });
        project_list_failure_ = last_failure_;
        unavailable_failure_ = project_list_failure_;
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
    entry->observed_md5 = std::string(data->md5);
    entry->observed_version.reset();
    entry->observed_at_unix_millis = access_activation_unix_millis(*loop_);
    entry->last_failure.reset();
    entry->config_state = AccessProjectConfigState::Processing;
    publish_readiness();

    if (data->state == nacos::ConfigState::NotFound || data->content.empty()) {
        if (!initial_snapshot_published_) {
            report_failure(entry, AccessConfigWatcherFailureStage::Decode,
                           options_.project_route_data_id_prefix + entry->project, std::string(data->md5),
                           data->state == nacos::ConfigState::NotFound ? common::IoErr::NotFound
                                                                       : common::IoErr::Invalid,
                           AccessConfigError{
                                   .code = AccessConfigErrorCode::InvalidCombination,
                                   .field = "route",
                                   .message = data->state == nacos::ConfigState::NotFound
                                                      ? "initial project route configuration was not found"
                                                      : "initial project route configuration is empty",
                           });
            settle_project(entry, AccessProjectConfigState::Rejected);
            return;
        }
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
    compile_project_inline(entry, std::move(data));
}

void AccessConfigWatcher::compile_project_inline(const std::shared_ptr<ProjectEntry> &entry,
                                                 std::shared_ptr<const nacos::ConfigData> data, bool force_compile) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    FIBER_ASSERT(data);
    // Compiles synchronously on this (nacos) loop — no cross-loop job
    // protocol. One bounded re-pass covers the corner where the compiler
    // skipped a same-version candidate whose snapshot is no longer the loaded
    // one: force_compile makes the skip branch unreachable on the second
    // pass, so the loop runs at most twice. Unreachable in practice because
    // replay callers already pass force_compile.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto found = projects_.find(entry->project);
        if (state_ != AccessConfigWatcherState::Running || found == projects_.end() ||
            found->second.get() != entry.get() || entry->subscription.state() == SubscriptionLifecycleState::Stopped) {
            return;
        }
        const std::uint64_t generation = entry->subscription.generation();
        const std::optional<std::int32_t> published_version = store_->current_version(entry->project);
        const auto started = std::chrono::steady_clock::now();
        CompiledProjectConfigResult result =
                compiler_->compile_project(entry->project, data->content, published_version, force_compile);
        const auto duration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        observe_metric_duration(AccessConfigMetricStage::ProjectCompile, duration);
        if (duration > kProjectCompileWarnThreshold) {
            LOG(LOG_CONFIG, WARN) << "project_compile_slow"
                                  << " project=\"" << entry->project << "\""
                                  << " duration_ms="
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        }
        if (result && result->compilation_skipped &&
            (!result->version || store_->current_version(entry->project) != result->version)) {
            force_compile = true;
            continue;
        }
        apply_compiled_project(entry, data, generation, std::move(result));
        return;
    }
}

void AccessConfigWatcher::apply_compiled_project(const std::shared_ptr<ProjectEntry> &entry,
                                                 const std::shared_ptr<const nacos::ConfigData> &data,
                                                 std::uint64_t generation, CompiledProjectConfigResult result) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    FIBER_ASSERT(data);
    if (!result) {
        entry->observed_version = result.error().observed_version;
        const AccessConfigWatcherFailureStage stage = result.error().stage == AccessProjectCompileFailureStage::Decode
                                                              ? AccessConfigWatcherFailureStage::Decode
                                                              : AccessConfigWatcherFailureStage::Compile;
        report_failure(entry, stage, options_.project_route_data_id_prefix + entry->project, std::string(data->md5),
                       common::IoErr::Invalid, std::move(result.error().error));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }

    entry->observed_version = result->version;
    if (result->compilation_skipped) {
        // Reached only when the skipped version is still the loaded one; the
        // no-longer-loaded replay is handled by compile_project_inline's
        // bounded re-pass.
        FIBER_ASSERT(result->version && store_->current_version(entry->project) == result->version);
        observe_metric_event(AccessConfigMetricEvent::ProjectRouteVersionUnchanged);
        settle_project(entry, AccessProjectConfigState::Accepted);
        return;
    }
    auto prepared = store_->prepare_compiled(entry->project, result->version, std::move(result->snapshot));
    if (!prepared) {
        if (prepared.error().code == AccessConfigErrorCode::MissingDependency) {
            entry->retry_identity_data = data;
        } else {
            entry->retry_identity_data.reset();
        }
        report_failure(entry, AccessConfigWatcherFailureStage::Compile,
                       options_.project_route_data_id_prefix + entry->project, std::string(data->md5),
                       common::IoErr::Invalid, std::move(prepared.error()));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }
    entry->retry_identity_data.reset();
    apply_prepared_project(entry, std::move(*prepared), generation, entry->subscription.revision_version(),
                           options_.project_route_data_id_prefix + entry->project, std::string(data->md5));
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
    // Load-bearing now that compilation is inline: settle/commit paths called
    // from inside reconcile_projects or a nested compile reach here
    // re-entrantly; only the outermost, non-deferred invocation may commit
    // the batch.
    if (!initial_batch_active_ || defer_readiness_updates_ || unavailable_failure_ ||
        state_ != AccessConfigWatcherState::Running) {
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

    for (const auto &[project, entry]: projects_) {
        (void) project;
        if (entry->config_state != AccessProjectConfigState::Rejected) {
            continue;
        }
        if (entry->last_failure) {
            unavailable_failure_ = entry->last_failure;
        } else {
            set_unavailable(options_.project_route_data_id_prefix + entry->project, common::IoErr::Invalid,
                            "initial project route configuration was rejected");
            return;
        }
        publish_readiness();
        return;
    }

    struct PendingResult {
        std::shared_ptr<ProjectEntry> entry;
        std::uint64_t generation = 0;
        std::string data_id;
        std::string md5;
    };

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
        set_unavailable(options_.project_list_data_id, common::IoErr::Invalid,
                        "initial access configuration produced no route candidates");
        publish_readiness();
        return;
    }

    auto updated = store_->commit_batch(std::move(ready), ConfigBatchCommitMode::RequireAllProjects);
    defer_readiness_updates_ = true;
    if (!updated) {
        for (PendingResult &item: pending) {
            report_failure(item.entry, AccessConfigWatcherFailureStage::Publish, std::move(item.data_id),
                           std::move(item.md5), common::IoErr::Invalid, updated.error());
            item.entry->config_state = AccessProjectConfigState::Rejected;
            item.entry->synchronized = true;
        }
        if (!pending.empty()) {
            unavailable_failure_ = pending.front().entry->last_failure;
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
    if (!updated || !updated->published || updated->snapshot->host_count() == 0) {
        if (!unavailable_failure_) {
            set_unavailable(options_.project_list_data_id, common::IoErr::Invalid,
                            "initial access configuration produced no routable hosts");
        } else {
            publish_readiness();
        }
        return;
    }
    initial_batch_active_ = false;
    initial_snapshot_published_ = true;
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
        // Permanent subscription failure (closed subscription or non-retryable
        // error). Transient retry rounds stay unlogged to bound noise during
        // Nacos outages; they remain visible via retrying readiness counts.
        LOG(LOG_CONFIG, WARN) << "project_subscription_failed" << " project=\"" << entry->project << "\""
                              << " io_error=" << common::io_err_name(io_error) << " error=\""
                              << entry->last_failure->error.message << "\"";
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
    } else if (initial_batch_active_ && entry->last_failure) {
        unavailable_failure_ = entry->last_failure;
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
    if (unavailable_failure_ && unavailable_failure_->error.code == AccessConfigErrorCode::MissingDependency) {
        unavailable_failure_.reset();
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
        compile_project_inline(entry, entry->retry_identity_data, true);
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
    // Load-bearing now that compilation is inline: intermediate readiness
    // states would otherwise be observable from nested calls inside
    // reconcile_projects / commit_initial_batch_if_ready stacks.
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
    } else if (!initial_snapshot_published_ && project_list_failure_) {
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
    if (entry == nullptr) {
        // Project-list failures are terminal for the root subscription and rare.
        LOG(LOG_CONFIG, WARN) << "project_list_failed" << " data_id=\"" << last_failure_->data_id << "\""
                              << " stage=" << failure_stage_name(stage) << " field=\"" << last_failure_->error.field
                              << "\""
                              << " offset=" << last_failure_->error.offset << " error=\""
                              << last_failure_->error.message << "\"";
    } else if (stage != AccessConfigWatcherFailureStage::Subscription) {
        // Transient per-project subscription failures retry with backoff and only
        // log when they turn permanent (handle_subscription_failure); every other
        // stage settles the candidate terminally.
        LOG(LOG_CONFIG, WARN) << "project_config_failed" << " project=\"" << entry->project << "\""
                              << " data_id=\"" << last_failure_->data_id << "\""
                              << " md5=\"" << last_failure_->md5 << "\""
                              << " stage=" << failure_stage_name(stage) << " field=\"" << last_failure_->error.field
                              << "\""
                              << " offset=" << last_failure_->error.offset << " error=\""
                              << last_failure_->error.message << "\"";
    }
}

} // namespace fiber::access_server
