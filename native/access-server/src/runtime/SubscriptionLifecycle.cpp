#include "SubscriptionLifecycle.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

SubscriptionLifecycle::SubscriptionLifecycle(event::EventLoop &loop) noexcept : loop_(&loop) {
    revision_publisher_ = revisions_.acquire_publisher();
    FIBER_ASSERT(revision_publisher_.has_value());
}

SubscriptionLifecycle::~SubscriptionLifecycle() noexcept {
    FIBER_ASSERT(state_ == SubscriptionLifecycleState::Created || state_ == SubscriptionLifecycleState::Stopped);
    FIBER_ASSERT(!subscription_);
}

std::expected<void, nacos::ConfigServiceError>
SubscriptionLifecycle::subscribe(nacos::ConfigService &service, std::string_view data_id, std::string_view group,
                                 nacos::Subscription<nacos::ConfigData>::NotifyCallback on_notify, void *context) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == SubscriptionLifecycleState::Created || state_ == SubscriptionLifecycleState::Retrying ||
                 state_ == SubscriptionLifecycleState::Failed);
    FIBER_ASSERT(!subscription_);
    state_ = SubscriptionLifecycleState::Subscribing;
    next_retry_at_ = {};

    auto subscription = service.subscribe(data_id, group, on_notify, context);
    if (!subscription) {
        nacos::ConfigServiceError error = std::move(subscription.error());
        fail(error);
        return std::unexpected(std::move(error));
    }

    // ConfigService may replay a cached value, including Closed, before
    // subscribe() returns. Only install the handle when that callback left the
    // attempt active.
    if (state_ == SubscriptionLifecycleState::Subscribing) {
        subscription_ = std::move(*subscription);
        state_ = SubscriptionLifecycleState::Subscribed;
        retry_attempt_ = 0;
        next_retry_at_ = {};
    } else {
        subscription->close();
    }
    return {};
}

std::uint64_t SubscriptionLifecycle::observe_value() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == SubscriptionLifecycleState::Subscribing || state_ == SubscriptionLifecycleState::Subscribed);
    advance();
    first_value_received_ = true;
    last_failure_.reset();
    return generation_;
}

void SubscriptionLifecycle::fail(const nacos::ConfigServiceError &error) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == SubscriptionLifecycleState::Subscribing || state_ == SubscriptionLifecycleState::Subscribed);
    advance();
    subscription_.close();
    first_value_received_ = false;
    next_retry_at_ = {};
    last_failure_ = SubscriptionFailure{
            .code = error.code,
            .io_error = error.io_error,
    };
    state_ = SubscriptionLifecycleState::Failed;
}

std::optional<SubscriptionRetryPlan> SubscriptionLifecycle::schedule_retry(SubscriptionRetryPolicy policy) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == SubscriptionLifecycleState::Failed);
    FIBER_ASSERT(last_failure_);
    FIBER_ASSERT(policy.initial_delay >= std::chrono::milliseconds::zero());
    FIBER_ASSERT(policy.maximum_delay >= policy.initial_delay);
    if (!retryable(last_failure_->code)) {
        return std::nullopt;
    }

    FIBER_ASSERT(retry_attempt_ != std::numeric_limits<std::uint32_t>::max());
    const std::chrono::milliseconds delay = retry_delay(policy, ++retry_attempt_);
    state_ = SubscriptionLifecycleState::Retrying;
    next_retry_at_ = event::EventLoop::current().now() + delay;
    return SubscriptionRetryPlan{
            .delay = delay,
            .revision_version = revisions_.current().version,
    };
}

void SubscriptionLifecycle::reset_start_failure() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == SubscriptionLifecycleState::Failed);
    FIBER_ASSERT(!subscription_);
    state_ = SubscriptionLifecycleState::Created;
    first_value_received_ = false;
    retry_attempt_ = 0;
    next_retry_at_ = {};
    last_failure_.reset();
}

void SubscriptionLifecycle::stop() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == SubscriptionLifecycleState::Stopped) {
        return;
    }
    if (state_ != SubscriptionLifecycleState::Created) {
        advance();
    }
    subscription_.close();
    first_value_received_ = false;
    next_retry_at_ = {};
    state_ = SubscriptionLifecycleState::Stopped;
}

bool SubscriptionLifecycle::subscribed() const noexcept {
    FIBER_ASSERT(loop_->in_loop());
    return state_ == SubscriptionLifecycleState::Subscribed && subscription_ && !subscription_.closed();
}

void SubscriptionLifecycle::advance() noexcept {
    FIBER_ASSERT(generation_ != std::numeric_limits<std::uint64_t>::max());
    revision_publisher_->publish(++generation_);
}

bool SubscriptionLifecycle::retryable(nacos::ConfigServiceErrorCode code) noexcept {
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

std::chrono::milliseconds SubscriptionLifecycle::retry_delay(SubscriptionRetryPolicy policy,
                                                             std::uint32_t attempt) noexcept {
    std::chrono::milliseconds delay = policy.initial_delay;
    for (std::uint32_t current = 1; current < attempt && delay < policy.maximum_delay; ++current) {
        if (delay.count() > policy.maximum_delay.count() / 2) {
            return policy.maximum_delay;
        }
        delay *= 2;
    }
    return std::min(delay, policy.maximum_delay);
}

} // namespace fiber::access_server
