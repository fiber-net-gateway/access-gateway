#ifndef FIBER_ACCESS_SERVER_TLS_CERTIFICATE_WATCHER_H
#define FIBER_ACCESS_SERVER_TLS_CERTIFICATE_WATCHER_H

#include "TlsCertificateStore.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

enum class TlsCertificateWatcherState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct TlsCertificateWatcherOptions {
    std::string data_id = std::string(kTlsCertificatesDataId);
    std::string group = std::string(kTlsCertificatesGroup);
};

struct TlsCertificateWatcherFailure {
    std::string md5;
    TlsCertificateConfigError error;
};

class TlsCertificateWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    TlsCertificateWatcher(event::EventLoop &loop, nacos::ConfigService &config_service, TlsCertificateStore &store,
                          TlsCertificateWatcherOptions options = {});
    ~TlsCertificateWatcher();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_ready() { return ready_.subscribe(); }

    [[nodiscard]] TlsCertificateWatcherState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<TlsCertificateWatcherFailure> &last_failure() const noexcept {
        return last_failure_;
    }

private:
    static void on_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    void apply(const nacos::ConfigData &data);
    void request_stop() noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    TlsCertificateStore *store_ = nullptr;
    TlsCertificateWatcherOptions options_;
    std::optional<nacos::Subscription<nacos::ConfigData>> subscription_;
    std::optional<TlsCertificateWatcherFailure> last_failure_;
    async::Watch<bool> ready_{false};
    std::optional<async::Watch<bool>::Publisher> ready_publisher_;
    TlsCertificateWatcherState state_ = TlsCertificateWatcherState::Created;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TLS_CERTIFICATE_WATCHER_H
