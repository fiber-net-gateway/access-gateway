#include "TlsCertificateWatcher.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

TlsCertificateWatcher::TlsCertificateWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                             TlsCertificateStore &store, TlsCertificateWatcherOptions options) :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)) {
    ready_publisher_ = ready_.acquire_publisher();
    FIBER_ASSERT(ready_publisher_.has_value());
}

TlsCertificateWatcher::~TlsCertificateWatcher() {
    FIBER_ASSERT(state_ == TlsCertificateWatcherState::Created || state_ == TlsCertificateWatcherState::Stopped);
    FIBER_ASSERT(!subscription_);
}

std::expected<void, nacos::ConfigServiceError> TlsCertificateWatcher::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != TlsCertificateWatcherState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "TLS certificate watcher is already started",
        });
    }
    state_ = TlsCertificateWatcherState::Running;
    auto subscription = config_service_->subscribe(options_.data_id, options_.group, &on_notify, this);
    if (!subscription) {
        state_ = TlsCertificateWatcherState::Created;
        return std::unexpected(std::move(subscription.error()));
    }
    subscription_.emplace(std::move(*subscription));
    return {};
}

async::Task<void> TlsCertificateWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == TlsCertificateWatcherState::Stopped) {
        co_return;
    }
    if (state_ == TlsCertificateWatcherState::Created) {
        state_ = TlsCertificateWatcherState::Stopped;
        co_return;
    }
    state_ = TlsCertificateWatcherState::Stopping;
    request_stop();
    subscription_.reset();
    state_ = TlsCertificateWatcherState::Stopped;
}

void TlsCertificateWatcher::on_notify(void *context,
                                      const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<TlsCertificateWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        owner.request_stop();
        return;
    }
    if (result.data && owner.state_ == TlsCertificateWatcherState::Running) {
        owner.apply(*result.data);
    }
}

void TlsCertificateWatcher::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (data.state == nacos::ConfigState::NotFound || data.content.empty()) {
        return;
    }
    auto parsed = parse_tls_certificate_config(data.content);
    if (!parsed || !*parsed) {
        if (!parsed) {
            ++failed_updates_;
            last_failure_ = TlsCertificateWatcherFailure{
                    .md5 = std::string(data.md5),
                    .error = std::move(parsed.error()),
            };
        }
        return;
    }
    auto updated = store_->apply(**parsed, data.content);
    if (!updated) {
        ++failed_updates_;
        last_failure_ = TlsCertificateWatcherFailure{
                .md5 = std::string(data.md5),
                .error = std::move(updated.error()),
        };
        return;
    }
    if (*updated == TlsCertificateUpdateStatus::Published) {
        ++successful_updates_;
        ready_publisher_->publish(true);
    }
}

void TlsCertificateWatcher::request_stop() noexcept {
    if (subscription_) {
        subscription_->close();
    }
}

} // namespace fiber::access_server
