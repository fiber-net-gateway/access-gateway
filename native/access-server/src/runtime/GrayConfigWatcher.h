#ifndef FIBER_ACCESS_SERVER_GRAY_CONFIG_WATCHER_H
#define FIBER_ACCESS_SERVER_GRAY_CONFIG_WATCHER_H

#include "../observability/AccessActivationEvidence.h"
#include "GrayMatchStore.h"
#include "SubscriptionLifecycle.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

enum class GrayConfigWatcherState : std::uint8_t {
    Created,
    Running,
    Failed,
    Stopping,
    Stopped,
};

struct GrayConfigWatcherOptions {
    std::string data_id = std::string(kGrayConfigDataId);
    std::string group = std::string(kDefaultNacosGroup);
};

struct GrayConfigWatcherFailure {
    std::string stage;
    std::string code;
    std::string md5;
    AccessConfigError error;
    std::int64_t observed_at_unix_millis = 0;
};

class GrayConfigWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    GrayConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service, GrayMatchStore &store,
                      GrayConfigWatcherOptions options = {}, AccessGrayActivationEvidenceObserver observer = {});
    ~GrayConfigWatcher() noexcept;

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] GrayConfigWatcherState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<GrayConfigWatcherFailure> &last_failure() const noexcept { return last_failure_; }

private:
    static void on_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    void apply(const nacos::ConfigData &data);
    void publish_evidence() const noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    GrayMatchStore *store_ = nullptr;
    GrayConfigWatcherOptions options_;
    AccessGrayActivationEvidenceObserver observer_;
    SubscriptionLifecycle subscription_;
    std::optional<GrayConfigWatcherFailure> last_failure_;
    GrayConfigWatcherState state_ = GrayConfigWatcherState::Created;
    AccessActivationCandidateStatus candidate_status_ = AccessActivationCandidateStatus::Awaiting;
    std::string observed_md5_;
    std::string active_md5_;
    std::int64_t observed_at_unix_millis_ = 0;
    std::int64_t active_at_unix_millis_ = 0;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_GRAY_CONFIG_WATCHER_H
