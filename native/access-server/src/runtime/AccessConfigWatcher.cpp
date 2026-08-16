#include "AccessConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <algorithm>
#include <limits>
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
    bool first_value_received = false;
    bool synchronized = false;
    bool retry_scheduled = false;
    bool stopping = false;
};

AccessConfigWatcher::AccessConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                         RouteConfigStore &store, AccessConfigWatcherOptions options,
                                         RouteSnapshotObserver observer) :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)), observer_(observer) {
    readiness_publisher_ = readiness_.acquire_publisher();
    FIBER_ASSERT(readiness_publisher_.has_value());
}

AccessConfigWatcher::~AccessConfigWatcher() {
    FIBER_ASSERT(state_ == AccessConfigWatcherState::Created || state_ == AccessConfigWatcherState::Stopped);
    FIBER_ASSERT(project_list_ == nullptr);
    FIBER_ASSERT(projects_.empty());
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
        entry->subscription_state = AccessProjectSubscriptionState::Retiring;
        request_stop(*entry);
    }
    co_await background_tasks_.join();
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
        owner.apply_project(hold, *result.data);
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
    publish_readiness();
}

void AccessConfigWatcher::apply_project(const std::shared_ptr<ProjectEntry> &entry, const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    entry->advance();
    entry->first_value_received = true;
    entry->observed_md5 = std::string(data.md5);
    entry->observed_version.reset();
    entry->config_state = AccessProjectConfigState::Processing;
    publish_readiness();

    if (data.state == nacos::ConfigState::NotFound || data.content.empty()) {
        auto ignored = store_->prepare(entry->project, std::nullopt);
        FIBER_ASSERT(ignored.has_value());
        settle_project(entry, AccessProjectConfigState::Accepted);
        return;
    }

    auto parsed = parse_project_config(data.content);
    if (!parsed) {
        report_failure(entry, AccessConfigWatcherFailureStage::Decode,
                       options_.project_route_data_id_prefix + entry->project, std::string(data.md5),
                       common::IoErr::Invalid, std::move(parsed.error()));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }
    if (*parsed) {
        entry->observed_version = (*parsed)->version;
    }
    auto prepared = store_->prepare(entry->project, *parsed);
    if (!prepared) {
        report_failure(entry, AccessConfigWatcherFailureStage::Compile,
                       options_.project_route_data_id_prefix + entry->project, std::string(data.md5),
                       common::IoErr::Invalid, std::move(prepared.error()));
        settle_project(entry, AccessProjectConfigState::Rejected);
        return;
    }

    if (!prepared->needs_publish()) {
        settle_project(entry, AccessProjectConfigState::Accepted);
        return;
    }
    if (prepared->status == ConfigUpdateStatus::Unloaded) {
        auto updated = store_->commit(std::move(*prepared));
        if (!updated) {
            report_failure(entry, AccessConfigWatcherFailureStage::Publish,
                           options_.project_route_data_id_prefix + entry->project, std::string(data.md5),
                           common::IoErr::Invalid, std::move(updated.error()));
            settle_project(entry, AccessProjectConfigState::Rejected);
            return;
        }
        ++successful_updates_;
        entry->published_generation = entry->generation;
        publish_observer(updated->snapshot);
        settle_project(entry, AccessProjectConfigState::Accepted);
        return;
    }

    const std::uint64_t generation = entry->generation;
    const std::uint64_t revision_version = entry->revisions.current().version;
    std::string data_id = options_.project_route_data_id_prefix + entry->project;
    background_tasks_.add();
    async::spawn([this, entry, prepared = std::move(*prepared), generation, revision_version,
                  data_id = std::move(data_id), md5 = std::string(data.md5)]() mutable {
        return apply_ready_project(std::move(entry), std::move(prepared), generation, revision_version,
                                   std::move(data_id), std::move(md5));
    });
}

async::DetachedTask AccessConfigWatcher::apply_ready_project(std::shared_ptr<ProjectEntry> entry,
                                                             PreparedConfigUpdate prepared, std::uint64_t generation,
                                                             std::uint64_t revision_version, std::string data_id,
                                                             std::string md5) noexcept {
    auto revisions = entry->revisions.subscribe();
    auto ready_or_replaced =
            co_await async::when_any([&prepared]() { return prepared.project_snapshot->wait_ready().select(); },
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
            auto updated = store_->commit(std::move(prepared));
            if (!updated) {
                report_failure(entry, AccessConfigWatcherFailureStage::Publish, std::move(data_id), std::move(md5),
                               common::IoErr::Invalid, std::move(updated.error()));
                settle_project(entry, AccessProjectConfigState::Rejected);
            } else {
                ++successful_updates_;
                entry->published_generation = generation;
                publish_observer(updated->snapshot);
                settle_project(entry, AccessProjectConfigState::Accepted);
            }
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
    retiring->subscription_state = AccessProjectSubscriptionState::Retiring;
    request_stop(*retiring);

    auto removed = store_->remove_project(project);
    FIBER_ASSERT(removed.has_value());
    ++successful_updates_;
    publish_observer(removed->snapshot);
    publish_readiness();
}

void AccessConfigWatcher::subscribe_project(const std::shared_ptr<ProjectEntry> &entry) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry);
    if (state_ != AccessConfigWatcherState::Running || entry->stopping) {
        return;
    }

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

void AccessConfigWatcher::publish_observer(const std::shared_ptr<const AccessRouteSnapshot> &snapshot) const noexcept {
    if (observer_.on_update) {
        observer_.on_update(observer_.context, snapshot);
    }
}

void AccessConfigWatcher::report_failure(const std::shared_ptr<ProjectEntry> &entry,
                                         AccessConfigWatcherFailureStage stage, std::string data_id, std::string md5,
                                         common::IoErr io_error, AccessConfigError error) {
    ++failed_updates_;
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
