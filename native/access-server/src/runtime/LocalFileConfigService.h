#ifndef FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_SERVICE_H
#define FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_SERVICE_H

#include "../config/LocalFileConfigSource.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {

struct LocalFileConfigServiceOptions {
    std::string directory;
    LocalFileConfigLayout layout;
    // Zero disables periodic polling; reload() / trigger_reload() (e.g. SIGHUP)
    // still reloads on demand.
    std::chrono::milliseconds poll_interval{2000};
};

// Local file-backed ConfigService implementation feeding the exact same
// immutable snapshot compilation pipeline as Nacos without requiring rnacos.
//
// All ConfigService methods, reload(), and callbacks execute on `loop`.
// trigger_reload() may be called from any thread/loop (for example accept_loop
// upon SIGHUP) and posts reload() onto `loop`.
class LocalFileConfigService final : public nacos::ConfigService {
public:
    LocalFileConfigService(event::EventLoop &loop, LocalFileConfigServiceOptions options);
    ~LocalFileConfigService() noexcept override;

    [[nodiscard]] common::IoResult<void> start() noexcept override;
    [[nodiscard]] async::Task<void> shutdown() noexcept override;
    [[nodiscard]] StatusSubscriber subscribe_status() override;

    [[nodiscard]] async::Task<std::expected<std::shared_ptr<const nacos::ConfigData>, nacos::ConfigServiceError>>
    get_config(std::string data_id, std::string group) noexcept override;

    [[nodiscard]] async::Task<std::expected<void, nacos::ConfigServiceError>>
    publish(std::string data_id, std::string group, std::string content, nacos::ConfigType type,
            std::optional<std::string> cas_md5 = std::nullopt) noexcept override;

    [[nodiscard]] async::Task<std::expected<void, nacos::ConfigServiceError>>
    remove_config(std::string data_id, std::string group) noexcept override;

    [[nodiscard]] std::expected<nacos::Subscription<nacos::ConfigData>, nacos::ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group,
              nacos::Subscription<nacos::ConfigData>::NotifyCallback on_notify, void *ctx) override;

    // Owner-loop reload: reads the directory snapshot and pushes changed
    // documents to subscribed watchers. On read/parse failure, logs and retains
    // the previously loaded snapshot untouched.
    [[nodiscard]] std::expected<bool, LocalFileConfigError> reload() noexcept;

    // Cross-loop reload trigger (e.g. SIGHUP handler on accept_loop).
    void trigger_reload() noexcept;

    [[nodiscard]] std::uint64_t successful_reloads() const noexcept { return successful_reloads_; }
    [[nodiscard]] std::uint64_t failed_reloads() const noexcept { return failed_reloads_; }
    [[nodiscard]] const std::optional<LocalFileConfigError> &last_reload_error() const noexcept {
        return last_reload_error_;
    }

private:
    enum class State : std::uint8_t {
        Created,
        Running,
        Stopping,
        Stopped,
    };

    struct OwnedConfigData final {
        std::string md5;
        std::string content;
        nacos::ConfigData view;
    };

    struct SubscriptionNode final {
        LocalFileConfigService *service = nullptr;
        LocalFileConfigKey key;
        nacos::Subscription<nacos::ConfigData>::NotifyCallback on_notify = nullptr;
        void *ctx = nullptr;
        bool attached = false;
        bool closed = false;
    };

    static void close_subscription(void *node) noexcept;
    static bool subscription_closed(const void *node) noexcept;
    [[nodiscard]] static std::shared_ptr<const nacos::ConfigData> make_present_data(std::string md5,
                                                                                    std::string content);
    [[nodiscard]] static std::shared_ptr<const nacos::ConfigData> make_not_found_data();

    [[nodiscard]] async::DetachedTask poll_loop() noexcept;
    [[nodiscard]] int notification_priority(const LocalFileConfigKey &key) const noexcept;
    void notify_key(const LocalFileConfigKey &key, const std::shared_ptr<const nacos::ConfigData> &data) noexcept;

    event::EventLoop *loop_ = nullptr;
    LocalFileConfigSource source_;
    std::chrono::milliseconds poll_interval_{2000};
    State state_ = State::Created;
    async::Watch<nacos::ConfigServiceStatus> status_{nacos::ConfigServiceStatus{}};
    std::optional<async::Watch<nacos::ConfigServiceStatus>::Publisher> status_publisher_;
    async::Watch<bool> stop_signal_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    async::Watch<std::uint64_t> reload_trigger_{0};
    std::optional<async::Watch<std::uint64_t>::Publisher> reload_publisher_;
    std::atomic<std::uint64_t> reload_sequence_{0};
    async::WaitGroup background_tasks_;
    std::map<LocalFileConfigKey, std::shared_ptr<const nacos::ConfigData>> documents_;
    std::map<LocalFileConfigKey, std::vector<SubscriptionNode *>> subscribers_;
    std::shared_ptr<const nacos::ConfigData> not_found_data_;
    std::uint64_t successful_reloads_ = 0;
    std::uint64_t failed_reloads_ = 0;
    std::optional<LocalFileConfigError> last_reload_error_;
};

// Standalone no-op NamingService used when access-server runs with the local
// file config provider (no rnacos). Instance registration succeeds locally
// without network traffic; service discovery subscriptions fail clearly so
// routes relying on Nacos discovery instead of static `addresses` fail closed.
class LocalStandaloneNamingService final : public nacos::NamingService {
public:
    explicit LocalStandaloneNamingService(event::EventLoop &loop);
    ~LocalStandaloneNamingService() noexcept override;

    [[nodiscard]] common::IoResult<void> start() noexcept override;
    [[nodiscard]] async::Task<void> shutdown() noexcept override;
    [[nodiscard]] StatusSubscriber subscribe_status() override;

    [[nodiscard]] async::Task<std::expected<std::shared_ptr<const nacos::ServiceInfo>, nacos::NamingServiceError>>
    get(std::string service_name, std::string group) noexcept override;

    [[nodiscard]] std::expected<nacos::Subscription<nacos::ServiceInfo>, nacos::NamingServiceError>
    subscribe(std::string_view service_name, std::string_view group,
              nacos::Subscription<nacos::ServiceInfo>::NotifyCallback on_notify, void *ctx) override;

    [[nodiscard]] std::expected<nacos::InstanceRegistration, nacos::NamingServiceError>
    registry(std::string_view service_name, std::string_view group, nacos::Instance instance) override;

private:
    struct RegistrationStateBox final {
        async::Watch<nacos::RegistrationStatus> status{
                nacos::RegistrationStatus{.state = nacos::RegistrationState::Registered}};
        std::optional<async::Watch<nacos::RegistrationStatus>::Publisher> publisher;
    };

    static std::expected<void, nacos::NamingServiceError> update_registration(void *ctx,
                                                                              nacos::Instance instance) noexcept;
    static nacos::InstanceRegistration::StatusSubscriber subscribe_registration_status(void *ctx);
    static void close_registration(void *ctx) noexcept;

    event::EventLoop *loop_ = nullptr;
    async::Watch<nacos::NamingServiceStatus> status_{nacos::NamingServiceStatus{}};
    std::optional<async::Watch<nacos::NamingServiceStatus>::Publisher> status_publisher_;
    bool stopped_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_SERVICE_H
