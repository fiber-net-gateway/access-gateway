#include "GrayConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

std::string_view watcher_state_name(GrayConfigWatcherState state) noexcept {
    switch (state) {
        case GrayConfigWatcherState::Created:
            return "created";
        case GrayConfigWatcherState::Running:
            return "running";
        case GrayConfigWatcherState::Failed:
            return "failed";
        case GrayConfigWatcherState::Stopping:
            return "stopping";
        case GrayConfigWatcherState::Stopped:
            return "stopped";
    }
    return "created";
}

std::string_view error_code_name(AccessConfigErrorCode code) noexcept {
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
    }
    return "invalid_configuration";
}

} // namespace

GrayConfigWatcher::GrayConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                     GrayMatchStore &store, GrayConfigWatcherOptions options,
                                     AccessGrayActivationEvidenceObserver observer) :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)), observer_(observer),
    subscription_(loop) {}

GrayConfigWatcher::~GrayConfigWatcher() noexcept {
    FIBER_ASSERT(state_ == GrayConfigWatcherState::Created || state_ == GrayConfigWatcherState::Stopped);
}

std::expected<void, nacos::ConfigServiceError> GrayConfigWatcher::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != GrayConfigWatcherState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "gray config watcher is already started",
        });
    }
    state_ = GrayConfigWatcherState::Running;
    auto subscribed = subscription_.subscribe(*config_service_, options_.data_id, options_.group, &on_notify, this);
    if (!subscribed) {
        state_ = GrayConfigWatcherState::Created;
        subscription_.reset_start_failure();
        return std::unexpected(std::move(subscribed.error()));
    }
    publish_evidence();
    return {};
}

async::Task<void> GrayConfigWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == GrayConfigWatcherState::Stopped) {
        co_return;
    }
    if (state_ == GrayConfigWatcherState::Created) {
        subscription_.stop();
        state_ = GrayConfigWatcherState::Stopped;
        publish_evidence();
        co_return;
    }
    if (state_ == GrayConfigWatcherState::Running) {
        state_ = GrayConfigWatcherState::Stopping;
    }
    subscription_.stop();
    state_ = GrayConfigWatcherState::Stopped;
    publish_evidence();
}

void GrayConfigWatcher::on_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<GrayConfigWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        if (owner.state_ == GrayConfigWatcherState::Running) {
            owner.subscription_.fail(nacos::ConfigServiceError{
                    .code = nacos::ConfigServiceErrorCode::Shutdown,
                    .io_error = common::IoErr::NotConnected,
                    .message = "gray configuration subscription closed before shutdown",
            });
            owner.state_ = GrayConfigWatcherState::Failed;
            ++owner.failed_updates_;
            owner.last_failure_ = GrayConfigWatcherFailure{
                    .stage = "subscription",
                    .code = "subscription_closed",
                    .md5 = owner.observed_md5_,
                    .error =
                            AccessConfigError{
                                    .code = AccessConfigErrorCode::InvalidCombination,
                                    .field = "subscription",
                                    .message = "gray configuration subscription closed before shutdown",
                            },
                    .observed_at_unix_millis = access_activation_unix_millis(*owner.loop_),
            };
            owner.publish_evidence();
        }
        return;
    }
    if (result.data && owner.state_ == GrayConfigWatcherState::Running) {
        (void) owner.subscription_.observe_value();
        owner.apply(*result.data);
    }
}

void GrayConfigWatcher::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    observed_md5_ = std::string(data.md5);
    observed_at_unix_millis_ = access_activation_unix_millis(*loop_);
    candidate_status_ = AccessActivationCandidateStatus::Processing;
    last_failure_.reset();
    const std::string_view content =
            data.state == nacos::ConfigState::NotFound ? std::string_view{} : std::string_view(data.content);
    auto parsed = parse_gray_match_config(content);
    if (!parsed) {
        ++failed_updates_;
        candidate_status_ = AccessActivationCandidateStatus::Rejected;
        last_failure_ = GrayConfigWatcherFailure{
                .stage = "decode",
                .code = std::string(error_code_name(parsed.error().code)),
                .md5 = std::string(data.md5),
                .error = std::move(parsed.error()),
                .observed_at_unix_millis = observed_at_unix_millis_,
        };
        publish_evidence();
        return;
    }
    auto updated = store_->apply(*parsed);
    FIBER_ASSERT(updated.has_value());
    if (*updated == GrayMatchUpdateStatus::Published) {
        ++successful_updates_;
        active_md5_ = observed_md5_;
        active_at_unix_millis_ = observed_at_unix_millis_;
    }
    candidate_status_ = AccessActivationCandidateStatus::Accepted;
    publish_evidence();
}

void GrayConfigWatcher::publish_evidence() const noexcept {
    if (!observer_.on_update) {
        return;
    }
    AccessGrayActivationEvidence evidence{
            .watcher_state = std::string(watcher_state_name(state_)),
            .resource =
                    AccessActivationResourceEvidence{
                            .data_id = options_.data_id,
                            .group = options_.group,
                            .candidate_status = candidate_status_,
                            .observed_md5 = observed_md5_,
                            .active_md5 = active_md5_,
                            .observed_at_unix_millis = observed_at_unix_millis_,
                            .active_at_unix_millis = active_at_unix_millis_,
                    },
            .generation = store_->generation(),
            .rule_count = store_->rule_count(),
    };
    if (last_failure_) {
        evidence.resource.failure = AccessActivationFailure{
                .stage = last_failure_->stage,
                .code = last_failure_->code,
                .field = last_failure_->error.field,
                .offset = last_failure_->error.offset,
                .observed_at_unix_millis = last_failure_->observed_at_unix_millis,
        };
    }
    observer_.on_update(observer_.context, evidence);
}

} // namespace fiber::access_server
