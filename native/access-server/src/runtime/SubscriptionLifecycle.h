#ifndef FIBER_ACCESS_SERVER_SUBSCRIPTION_LIFECYCLE_H
#define FIBER_ACCESS_SERVER_SUBSCRIPTION_LIFECYCLE_H

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

namespace fiber::access_server {

enum class SubscriptionLifecycleState : std::uint8_t {
    Created,
    Subscribing,
    Subscribed,
    Retrying,
    Failed,
    Stopped,
};

struct SubscriptionRetryPolicy {
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds maximum_delay{30000};
};

struct SubscriptionRetryPlan {
    std::chrono::milliseconds delay{0};
    std::uint64_t revision_version = 0;
};

struct SubscriptionFailure {
    nacos::ConfigServiceErrorCode code = nacos::ConfigServiceErrorCode::Protocol;
    common::IoErr io_error = common::IoErr::None;
};

// Owner-loop-only state for one ConfigService subscription. Resource-specific
// decode, compile, readiness, publication, and failure projection remain in the
// composing watcher.
class SubscriptionLifecycle final : public common::NonCopyable, public common::NonMovable {
public:
    explicit SubscriptionLifecycle(event::EventLoop &loop) noexcept;
    ~SubscriptionLifecycle() noexcept;

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError>
    subscribe(nacos::ConfigService &service, std::string_view data_id, std::string_view group,
              nacos::Subscription<nacos::ConfigData>::NotifyCallback on_notify, void *context);

    [[nodiscard]] std::uint64_t observe_value() noexcept;
    void fail(const nacos::ConfigServiceError &error) noexcept;
    [[nodiscard]] std::optional<SubscriptionRetryPlan> schedule_retry(SubscriptionRetryPolicy policy) noexcept;
    void reset_start_failure() noexcept;
    void stop() noexcept;

    [[nodiscard]] async::Watch<std::uint64_t>::Subscriber subscribe_revisions() { return revisions_.subscribe(); }
    [[nodiscard]] SubscriptionLifecycleState state() const noexcept { return state_; }
    [[nodiscard]] bool subscribed() const noexcept;
    [[nodiscard]] bool first_value_received() const noexcept { return first_value_received_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t revision_version() const noexcept { return revisions_.current().version; }
    [[nodiscard]] bool is_current(std::uint64_t generation) const noexcept { return generation_ == generation; }
    [[nodiscard]] std::uint32_t retry_attempt() const noexcept { return retry_attempt_; }
    [[nodiscard]] std::chrono::steady_clock::time_point next_retry_at() const noexcept { return next_retry_at_; }
    [[nodiscard]] const std::optional<SubscriptionFailure> &last_failure() const noexcept { return last_failure_; }

private:
    void advance() noexcept;
    [[nodiscard]] static bool retryable(nacos::ConfigServiceErrorCode code) noexcept;
    [[nodiscard]] static std::chrono::milliseconds retry_delay(SubscriptionRetryPolicy policy,
                                                               std::uint32_t attempt) noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::Subscription<nacos::ConfigData> subscription_;
    async::Watch<std::uint64_t> revisions_{0};
    std::optional<async::Watch<std::uint64_t>::Publisher> revision_publisher_;
    std::optional<SubscriptionFailure> last_failure_;
    std::chrono::steady_clock::time_point next_retry_at_{};
    std::uint64_t generation_ = 0;
    std::uint32_t retry_attempt_ = 0;
    SubscriptionLifecycleState state_ = SubscriptionLifecycleState::Created;
    bool first_value_received_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_SUBSCRIPTION_LIFECYCLE_H
