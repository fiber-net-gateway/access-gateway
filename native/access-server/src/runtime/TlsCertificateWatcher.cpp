#include "TlsCertificateWatcher.h"

#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

struct TlsCertificateWatcher::CompileJob final : public common::NonCopyable, public common::NonMovable {
    TlsCertificateWatcher *owner = nullptr;
    std::shared_ptr<const nacos::ConfigData> data;
    TlsCertificateVersionState published_state;
    std::optional<CompiledTlsCertificateConfigResult> result;
    std::atomic<bool> canceled{false};
    std::uint64_t generation = 0;
    bool quic_enabled = false;
    bool prepare_bootstrap = false;
    bool force_compile = false;
    event::EventLoop::NotifyEntry compile_entry;
    event::EventLoop::NotifyEntry completion_entry;
};

TlsCertificateWatcher::TlsCertificateWatcher(event::EventLoop &loop, AccessConfigCompiler &compiler,
                                             nacos::ConfigService &config_service, TlsCertificateStore &store,
                                             TlsCertificateWatcherOptions options) :
    loop_(&loop), compiler_(&compiler), config_service_(&config_service), store_(&store), options_(std::move(options)) {
    FIBER_ASSERT(loop_ != &compiler_->loop());
    ready_publisher_ = ready_.acquire_publisher();
    FIBER_ASSERT(ready_publisher_.has_value());
    processing_publisher_ = processing_.acquire_publisher();
    FIBER_ASSERT(processing_publisher_.has_value());
}

TlsCertificateWatcher::~TlsCertificateWatcher() {
    FIBER_ASSERT(state_ == TlsCertificateWatcherState::Created || state_ == TlsCertificateWatcherState::Stopped);
    FIBER_ASSERT(!subscription_);
    FIBER_ASSERT(!pending_compile_data_);
    FIBER_ASSERT(active_compile_job_ == nullptr);
    FIBER_ASSERT(compile_tasks_.empty());
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
    FIBER_ASSERT(generation_ != std::numeric_limits<std::uint64_t>::max());
    ++generation_;
    cancel_compile();
    request_stop();
    co_await compile_tasks_.join();
    publish_processing(false);
    subscription_.reset();
    state_ = TlsCertificateWatcherState::Stopped;
}

void TlsCertificateWatcher::on_notify(void *context,
                                      const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<TlsCertificateWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        if (owner.state_ == TlsCertificateWatcherState::Running) {
            FIBER_ASSERT(owner.generation_ != std::numeric_limits<std::uint64_t>::max());
            ++owner.generation_;
            owner.cancel_compile();
        }
        owner.request_stop();
        return;
    }
    if (result.data && owner.state_ == TlsCertificateWatcherState::Running) {
        owner.apply(result.data);
    }
}

void TlsCertificateWatcher::apply(std::shared_ptr<const nacos::ConfigData> data) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(data);
    FIBER_ASSERT(generation_ != std::numeric_limits<std::uint64_t>::max());
    ++generation_;
    cancel_compile();
    if (data->state == nacos::ConfigState::NotFound || data->content.empty()) {
        return;
    }
    if (data->content.size() > kMaxTlsSnapshotBytes) {
        report_failure(std::string(data->md5), TlsCertificateConfigError{
                                                       .code = TlsCertificateConfigErrorCode::LimitExceeded,
                                                       .message = "TLS certificate snapshot exceeds 4 MiB",
                                               });
        return;
    }
    enqueue_compile(std::move(data));
}

void TlsCertificateWatcher::enqueue_compile(std::shared_ptr<const nacos::ConfigData> data, bool force_compile) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(data);
    pending_compile_data_ = std::move(data);
    pending_force_compile_ = force_compile;
    publish_processing(true);
    if (active_compile_job_) {
        active_compile_job_->canceled.store(true, std::memory_order_release);
        return;
    }
    dispatch_compile();
}

void TlsCertificateWatcher::dispatch_compile() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != TlsCertificateWatcherState::Running || active_compile_job_ || !pending_compile_data_) {
        return;
    }
    auto data = std::exchange(pending_compile_data_, {});
    const bool force_compile = std::exchange(pending_force_compile_, false);
    auto *job = new (std::nothrow) CompileJob();
    if (!job) {
        report_failure(std::string(data->md5), TlsCertificateConfigError{
                                                       .code = TlsCertificateConfigErrorCode::InvalidField,
                                                       .field = "compiler",
                                                       .message = "failed to allocate TLS compilation job",
                                               });
        publish_processing(false);
        return;
    }
    job->owner = this;
    job->data = std::move(data);
    job->published_state = store_->version_state();
    job->generation = generation_;
    job->quic_enabled = store_->quic_enabled();
    job->prepare_bootstrap = !store_->bootstrap_identity();
    job->force_compile = force_compile;
    active_compile_job_ = job;
    compile_tasks_.add();
    compiler_->loop().post<CompileJob, &CompileJob::compile_entry, &run_compile>(*job);
}

void TlsCertificateWatcher::run_compile(CompileJob *job) noexcept {
    FIBER_ASSERT(job);
    TlsCertificateWatcher &owner = *job->owner;
    FIBER_ASSERT(owner.compiler_->loop().in_loop());
    if (!job->canceled.load(std::memory_order_acquire)) {
        CompiledTlsCertificateConfigResult result =
                owner.compiler_->compile_tls(job->data->content, job->published_state, job->quic_enabled,
                                             job->prepare_bootstrap, job->force_compile);
        if (!job->canceled.load(std::memory_order_acquire)) {
            job->result.emplace(std::move(result));
        }
    }
    owner.loop_->post<CompileJob, &CompileJob::completion_entry, &complete_compile>(*job);
}

void TlsCertificateWatcher::complete_compile(CompileJob *job) noexcept {
    FIBER_ASSERT(job);
    std::unique_ptr<CompileJob> owned(job);
    TlsCertificateWatcher &owner = *job->owner;
    FIBER_ASSERT(owner.loop_->in_loop());
    FIBER_ASSERT(owner.active_compile_job_ == job);
    owner.active_compile_job_ = nullptr;
    const bool current = owner.state_ == TlsCertificateWatcherState::Running && owner.generation_ == job->generation &&
                         !job->canceled.load(std::memory_order_acquire) && job->result;
    if (current) {
        owner.apply_result(*job);
    }
    owner.compile_tasks_.done();
    owner.dispatch_compile();
    if (!owner.active_compile_job_ && !owner.pending_compile_data_) {
        owner.publish_processing(false);
    }
}

void TlsCertificateWatcher::cancel_compile() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (active_compile_job_) {
        active_compile_job_->canceled.store(true, std::memory_order_release);
    }
    pending_compile_data_.reset();
    pending_force_compile_ = false;
    if (!active_compile_job_) {
        publish_processing(false);
    }
}

void TlsCertificateWatcher::publish_processing(bool processing) {
    FIBER_ASSERT(loop_->in_loop());
    if (published_processing_ == processing) {
        return;
    }
    published_processing_ = processing;
    processing_publisher_->publish(processing);
}

void TlsCertificateWatcher::report_failure(std::string md5, TlsCertificateConfigError error) {
    ++failed_updates_;
    last_failure_ = TlsCertificateWatcherFailure{
            .md5 = std::move(md5),
            .error = std::move(error),
    };
}

void TlsCertificateWatcher::apply_result(CompileJob &job) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(job.result);
    CompiledTlsCertificateConfigResult result = std::move(*job.result);
    if (!result) {
        report_failure(std::string(job.data->md5), std::move(result.error()));
        return;
    }
    if (!result->version) {
        return;
    }

    if (result->compilation_skipped) {
        TlsCertificateClassification classified = store_->classify(*result->version, result->content_digest);
        if (!classified) {
            enqueue_compile(job.data, true);
            return;
        }
        if (!*classified) {
            report_failure(std::string(job.data->md5), std::move(classified->error()));
            return;
        }
        if (**classified == TlsCertificateUpdateStatus::Published) {
            ++successful_updates_;
            ready_publisher_->publish(true);
        }
        return;
    }
    if (!result->prepared) {
        report_failure(std::string(job.data->md5), TlsCertificateConfigError{
                                                           .code = TlsCertificateConfigErrorCode::InvalidField,
                                                           .field = "compiler",
                                                           .message = "TLS compiler returned no prepared snapshot",
                                                   });
        return;
    }
    auto updated = store_->commit(std::move(*result->prepared));
    if (!updated) {
        report_failure(std::string(job.data->md5), std::move(updated.error()));
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
