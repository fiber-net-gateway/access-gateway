#ifndef FIBER_ACCESS_SERVER_TLS_CERTIFICATE_WATCHER_H
#define FIBER_ACCESS_SERVER_TLS_CERTIFICATE_WATCHER_H

#include "../observability/AccessActivationEvidence.h"
#include "AccessConfigCompiler.h"
#include "TlsCertificateStore.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

enum class TlsCertificateWatcherState : std::uint8_t {
    Created,
    Running,
    Failed,
    Stopping,
    Stopped,
};

struct TlsCertificateWatcherOptions {
    std::string data_id = std::string(kTlsCertificatesDataId);
    std::string group = std::string(kTlsCertificatesGroup);
};

struct TlsCertificateWatcherFailure {
    std::string stage;
    std::string code;
    std::string md5;
    TlsCertificateConfigError error;
    std::int64_t observed_at_unix_millis = 0;
};

class TlsCertificateWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    TlsCertificateWatcher(event::EventLoop &loop, AccessConfigCompiler &compiler, nacos::ConfigService &config_service,
                          TlsCertificateStore &store, TlsCertificateWatcherOptions options = {},
                          AccessTlsActivationEvidenceObserver observer = {});
    ~TlsCertificateWatcher();

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_ready() { return ready_.subscribe(); }
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_processing() { return processing_.subscribe(); }

    [[nodiscard]] TlsCertificateWatcherState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<TlsCertificateWatcherFailure> &last_failure() const noexcept {
        return last_failure_;
    }

private:
    struct CompileJob;

    static void on_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    static void run_compile(CompileJob *job) noexcept;
    static void complete_compile(CompileJob *job) noexcept;
    void apply(std::shared_ptr<const nacos::ConfigData> data);
    void enqueue_compile(std::shared_ptr<const nacos::ConfigData> data, bool force_compile = false);
    void dispatch_compile();
    void cancel_compile() noexcept;
    void publish_processing(bool processing);
    void publish_evidence() const noexcept;
    void report_failure(std::string md5, TlsCertificateConfigError error);
    void apply_result(CompileJob &job);
    void request_stop() noexcept;

    event::EventLoop *loop_ = nullptr;
    AccessConfigCompiler *compiler_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    TlsCertificateStore *store_ = nullptr;
    TlsCertificateWatcherOptions options_;
    AccessTlsActivationEvidenceObserver observer_;
    std::optional<nacos::Subscription<nacos::ConfigData>> subscription_;
    std::optional<TlsCertificateWatcherFailure> last_failure_;
    std::shared_ptr<const nacos::ConfigData> pending_compile_data_;
    CompileJob *active_compile_job_ = nullptr;
    async::Watch<bool> ready_{false};
    std::optional<async::Watch<bool>::Publisher> ready_publisher_;
    async::Watch<bool> processing_{false};
    std::optional<async::Watch<bool>::Publisher> processing_publisher_;
    async::WaitGroup compile_tasks_;
    TlsCertificateWatcherState state_ = TlsCertificateWatcherState::Created;
    AccessActivationCandidateStatus candidate_status_ = AccessActivationCandidateStatus::Awaiting;
    std::string observed_md5_;
    std::string active_md5_;
    std::int64_t observed_at_unix_millis_ = 0;
    std::int64_t active_at_unix_millis_ = 0;
    std::uint64_t generation_ = 0;
    bool pending_force_compile_ = false;
    bool published_processing_ = false;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TLS_CERTIFICATE_WATCHER_H
