#include "TempClusterConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <string>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

TempClusterConfigWatcher::TempClusterConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                                   TempClusterStore &store,
                                                   TempClusterConfigWatcherOptions options) noexcept :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)), subscription_(loop) {
    readiness_publisher_ = readiness_.acquire_publisher();
    FIBER_ASSERT(readiness_publisher_);
}

TempClusterConfigWatcher::~TempClusterConfigWatcher() noexcept {
    FIBER_ASSERT(state_ == TempClusterConfigWatcherState::Created || state_ == TempClusterConfigWatcherState::Stopped);
}

std::expected<void, nacos::ConfigServiceError> TempClusterConfigWatcher::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != TempClusterConfigWatcherState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "temp-cluster config watcher is already started",
        });
    }
    state_ = TempClusterConfigWatcherState::Running;
    auto subscribed = subscription_.subscribe(*config_service_, options_.data_id, options_.group, &on_notify, this);
    if (!subscribed) {
        state_ = TempClusterConfigWatcherState::Created;
        subscription_.reset_start_failure();
        return std::unexpected(std::move(subscribed.error()));
    }
    return {};
}

async::Task<void> TempClusterConfigWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == TempClusterConfigWatcherState::Stopped) {
        co_return;
    }
    if (state_ == TempClusterConfigWatcherState::Running) {
        state_ = TempClusterConfigWatcherState::Stopping;
    }
    readiness_publisher_->publish(false);
    subscription_.stop();
    state_ = TempClusterConfigWatcherState::Stopped;
    co_return;
}

void TempClusterConfigWatcher::on_notify(void *context,
                                         const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<TempClusterConfigWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        if (owner.state_ == TempClusterConfigWatcherState::Running) {
            owner.subscription_.fail(nacos::ConfigServiceError{
                    .code = nacos::ConfigServiceErrorCode::Shutdown,
                    .io_error = common::IoErr::NotConnected,
                    .message = "temp-cluster configuration subscription closed before shutdown",
            });
            owner.state_ = TempClusterConfigWatcherState::Failed;
            owner.readiness_publisher_->publish(false);
            ++owner.failed_updates_;
            owner.last_failure_ = TempClusterConfigWatcherFailure{
                    .stage = "subscription",
                    .code = "subscription_closed",
                    .md5 = owner.observed_md5_,
                    .message = "temp-cluster configuration subscription closed before shutdown",
            };
        }
        return;
    }
    if (result.data && owner.state_ == TempClusterConfigWatcherState::Running) {
        (void) owner.subscription_.observe_value();
        owner.apply(*result.data);
    }
}

void TempClusterConfigWatcher::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    observed_md5_ = std::string(data.md5);
    last_failure_.reset();
    // Java maps the Nacos not-found state to an empty whitelist.
    const std::string_view content =
            data.state == nacos::ConfigState::NotFound ? std::string_view{} : std::string_view(data.content);
    auto parsed = parse_temp_cluster_config(content);
    if (!parsed) {
        // Java's updateNameClusterMap throws on invalid content, which keeps
        // the previous map: retain the active snapshot.
        ++failed_updates_;
        last_failure_ = TempClusterConfigWatcherFailure{
                .stage = "decode",
                .code = "invalid_configuration",
                .md5 = std::string(data.md5),
                .message = std::string(parsed.error().message),
        };
        return;
    }
    auto updated = store_->apply(*parsed);
    FIBER_ASSERT(updated.has_value());
    readiness_publisher_->publish(true);
    ++successful_updates_;
}

} // namespace fiber::access_server
