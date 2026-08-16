#include "TlsCertificateWatcher.h"

#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

std::string_view watcher_state_name(TlsCertificateWatcherState state) noexcept {
    switch (state) {
        case TlsCertificateWatcherState::Created:
            return "created";
        case TlsCertificateWatcherState::Running:
            return "running";
        case TlsCertificateWatcherState::Failed:
            return "failed";
        case TlsCertificateWatcherState::Stopping:
            return "stopping";
        case TlsCertificateWatcherState::Stopped:
            return "stopped";
    }
    return "created";
}

std::string_view error_code_name(TlsCertificateConfigErrorCode code) noexcept {
    switch (code) {
        case TlsCertificateConfigErrorCode::InvalidJson:
            return "invalid_json";
        case TlsCertificateConfigErrorCode::InvalidRoot:
            return "invalid_root";
        case TlsCertificateConfigErrorCode::InvalidField:
            return "invalid_field";
        case TlsCertificateConfigErrorCode::MissingField:
            return "missing_field";
        case TlsCertificateConfigErrorCode::DuplicateField:
            return "duplicate_field";
        case TlsCertificateConfigErrorCode::LimitExceeded:
            return "limit_exceeded";
        case TlsCertificateConfigErrorCode::InvalidCertificate:
            return "invalid_certificate";
        case TlsCertificateConfigErrorCode::InvalidPrivateKey:
            return "invalid_private_key";
        case TlsCertificateConfigErrorCode::InvalidDnsName:
            return "invalid_dns_name";
        case TlsCertificateConfigErrorCode::DuplicateDnsName:
            return "duplicate_dns_name";
        case TlsCertificateConfigErrorCode::DefaultCertificateNotFound:
            return "default_certificate_not_found";
        case TlsCertificateConfigErrorCode::VersionConflict:
            return "version_conflict";
    }
    return "invalid_tls_configuration";
}

} // namespace

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
                                             TlsCertificateWatcherOptions options,
                                             AccessTlsActivationEvidenceObserver observer) :
    loop_(&loop), compiler_(&compiler), config_service_(&config_service), store_(&store), options_(std::move(options)),
    observer_(observer) {
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
    publish_evidence();
    return {};
}

async::Task<void> TlsCertificateWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == TlsCertificateWatcherState::Stopped) {
        co_return;
    }
    if (state_ == TlsCertificateWatcherState::Created) {
        state_ = TlsCertificateWatcherState::Stopped;
        publish_evidence();
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
    publish_evidence();
}

void TlsCertificateWatcher::on_notify(void *context,
                                      const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<TlsCertificateWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        if (owner.state_ == TlsCertificateWatcherState::Running) {
            FIBER_ASSERT(owner.generation_ != std::numeric_limits<std::uint64_t>::max());
            ++owner.generation_;
            owner.cancel_compile();
            owner.state_ = TlsCertificateWatcherState::Failed;
            ++owner.failed_updates_;
            owner.last_failure_ = TlsCertificateWatcherFailure{
                    .stage = "subscription",
                    .code = "subscription_closed",
                    .md5 = owner.observed_md5_,
                    .error =
                            TlsCertificateConfigError{
                                    .code = TlsCertificateConfigErrorCode::InvalidField,
                                    .field = "subscription",
                                    .message = "TLS certificate subscription closed before shutdown",
                            },
                    .observed_at_unix_millis = access_activation_unix_millis(*owner.loop_),
            };
            owner.publish_evidence();
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
    observed_md5_ = std::string(data->md5);
    observed_at_unix_millis_ = access_activation_unix_millis(*loop_);
    candidate_status_ = AccessActivationCandidateStatus::Processing;
    last_failure_.reset();
    publish_evidence();
    if (data->state == nacos::ConfigState::NotFound || data->content.empty()) {
        candidate_status_ = AccessActivationCandidateStatus::Accepted;
        publish_evidence();
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
    candidate_status_ = AccessActivationCandidateStatus::Rejected;
    last_failure_ = TlsCertificateWatcherFailure{
            .stage = "compile",
            .code = std::string(error_code_name(error.code)),
            .md5 = std::move(md5),
            .error = std::move(error),
            .observed_at_unix_millis = access_activation_unix_millis(*loop_),
    };
    publish_evidence();
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
        candidate_status_ = AccessActivationCandidateStatus::Accepted;
        publish_evidence();
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
            active_md5_ = std::string(job.data->md5);
            active_at_unix_millis_ = access_activation_unix_millis(*loop_);
            ready_publisher_->publish(true);
        } else if (**classified == TlsCertificateUpdateStatus::VersionUnchanged) {
            active_md5_ = std::string(job.data->md5);
            active_at_unix_millis_ = access_activation_unix_millis(*loop_);
        }
        candidate_status_ = AccessActivationCandidateStatus::Accepted;
        publish_evidence();
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
        active_md5_ = std::string(job.data->md5);
        active_at_unix_millis_ = access_activation_unix_millis(*loop_);
        ready_publisher_->publish(true);
    }
    candidate_status_ = AccessActivationCandidateStatus::Accepted;
    publish_evidence();
}

void TlsCertificateWatcher::publish_evidence() const noexcept {
    if (!observer_.on_update) {
        return;
    }
    AccessTlsActivationEvidence evidence{
            .enabled = true,
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
            .version = store_->version(),
            .certificate_count = store_->certificate_count(),
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

void TlsCertificateWatcher::request_stop() noexcept {
    if (subscription_) {
        subscription_->close();
    }
}

} // namespace fiber::access_server
