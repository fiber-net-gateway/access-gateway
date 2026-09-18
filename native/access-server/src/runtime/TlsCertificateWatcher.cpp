#include "TlsCertificateWatcher.h"

#include "../observability/AccessServerLogCategories.h"

#include <chrono>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_CONFIG, kAccessServerConfigLogger);

// TLS snapshot compilation runs inline on the nacos loop; past this threshold
// it is worth a warning because it delays other loop work.
constexpr std::chrono::milliseconds kTlsCompileWarnThreshold{50};

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

nacos::ConfigServiceError subscription_closed_error() {
    return nacos::ConfigServiceError{
            .code = nacos::ConfigServiceErrorCode::Shutdown,
            .io_error = common::IoErr::NotConnected,
            .message = "TLS certificate subscription closed before shutdown",
    };
}

} // namespace

TlsCertificateWatcher::TlsCertificateWatcher(event::EventLoop &loop, AccessConfigCompiler &compiler,
                                             nacos::ConfigService &config_service, TlsCertificateStore &store,
                                             TlsCertificateWatcherOptions options,
                                             AccessTlsActivationEvidenceObserver observer) :
    loop_(&loop), compiler_(&compiler), config_service_(&config_service), store_(&store), options_(std::move(options)),
    observer_(observer), subscription_(loop) {
    // TLS snapshots compile inline on this loop; the compiler must be bound to
    // it so its loop-ownership assertions hold.
    FIBER_ASSERT(loop_ == &compiler_->loop());
    readiness_publisher_ = readiness_.acquire_publisher();
    FIBER_ASSERT(readiness_publisher_.has_value());
    processing_publisher_ = processing_.acquire_publisher();
    FIBER_ASSERT(processing_publisher_.has_value());
}

TlsCertificateWatcher::~TlsCertificateWatcher() noexcept {
    FIBER_ASSERT(state_ == TlsCertificateWatcherState::Created || state_ == TlsCertificateWatcherState::Stopped);
    FIBER_ASSERT(!starting_subscription_);
    FIBER_ASSERT(!startup_replay_data_);
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
    starting_subscription_ = true;
    auto subscribed = subscription_.subscribe(*config_service_, options_.data_id, options_.group, &on_notify, this);
    starting_subscription_ = false;
    if (!subscribed) {
        startup_replay_data_.reset();
        state_ = TlsCertificateWatcherState::Created;
        subscription_.reset_start_failure();
        return std::unexpected(std::move(subscribed.error()));
    }
    if (subscription_.state() == SubscriptionLifecycleState::Failed) {
        FIBER_ASSERT(!startup_replay_data_);
        state_ = TlsCertificateWatcherState::Created;
        subscription_.reset_start_failure();
        return std::unexpected(subscription_closed_error());
    }
    if (startup_replay_data_) {
        auto data = std::exchange(startup_replay_data_, {});
        (void) subscription_.observe_value();
        apply(std::move(data));
    }
    publish_evidence();
    return {};
}

async::Task<void> TlsCertificateWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == TlsCertificateWatcherState::Stopped) {
        co_return;
    }
    if (state_ == TlsCertificateWatcherState::Created) {
        subscription_.stop();
        state_ = TlsCertificateWatcherState::Stopped;
        publish_evidence();
        co_return;
    }
    state_ = TlsCertificateWatcherState::Stopping;
    subscription_.stop();
    publish_processing(false);
    state_ = TlsCertificateWatcherState::Stopped;
    publish_evidence();
}

void TlsCertificateWatcher::on_notify(void *context,
                                      const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<TlsCertificateWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        if (owner.state_ == TlsCertificateWatcherState::Running) {
            owner.subscription_.fail(subscription_closed_error());
            if (owner.starting_subscription_) {
                owner.startup_replay_data_.reset();
                return;
            }
            owner.state_ = TlsCertificateWatcherState::Failed;
            ++owner.failed_updates_;
            LOG(LOG_CONFIG, WARN) << "tls_subscription_failed"
                                  << " code=subscription_closed"
                                  << " md5=\"" << owner.observed_md5_ << "\"";
            if (owner.candidate_status_ == AccessActivationCandidateStatus::Processing) {
                owner.candidate_status_ = AccessActivationCandidateStatus::Rejected;
            }
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
            const auto readiness = owner.readiness_.current();
            if (!readiness.value || *readiness.value == TlsCertificateReadiness::Awaiting) {
                owner.readiness_publisher_->publish(TlsCertificateReadiness::Failed);
            }
            owner.publish_evidence();
        }
        return;
    }
    if (result.data && owner.state_ == TlsCertificateWatcherState::Running) {
        if (owner.starting_subscription_) {
            owner.startup_replay_data_ = result.data;
            return;
        }
        (void) owner.subscription_.observe_value();
        owner.apply(result.data);
    }
}

void TlsCertificateWatcher::apply(std::shared_ptr<const nacos::ConfigData> data) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(data);
    if (initial_rejected_) {
        return;
    }
    observed_md5_ = std::string(data->md5);
    observed_at_unix_millis_ = access_activation_unix_millis(*loop_);
    candidate_status_ = AccessActivationCandidateStatus::Processing;
    last_failure_.reset();
    publish_evidence();
    if (data->state == nacos::ConfigState::NotFound || data->content.empty()) {
        report_failure(std::string(data->md5),
                       TlsCertificateConfigError{
                               .code = TlsCertificateConfigErrorCode::MissingField,
                               .field = "certificates",
                               .message = data->state == nacos::ConfigState::NotFound
                                                  ? "TLS certificate configuration was not found"
                                                  : "TLS certificate configuration is empty",
                       });
        return;
    }
    if (data->content.size() > kMaxTlsSnapshotBytes) {
        report_failure(std::string(data->md5), TlsCertificateConfigError{
                                                       .code = TlsCertificateConfigErrorCode::LimitExceeded,
                                                       .message = "TLS certificate snapshot exceeds 4 MiB",
                                               });
        return;
    }
    compile_tls_inline(std::move(data));
}

void TlsCertificateWatcher::compile_tls_inline(std::shared_ptr<const nacos::ConfigData> data, bool force_compile) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(data);
    // Compiles synchronously on this (nacos) loop — no cross-loop job
    // protocol. One bounded re-pass covers the corner where the compiler
    // skipped a not-newer snapshot that is no longer confirmably the loaded
    // one: force_compile makes the skip branch unreachable on the second
    // pass. The processing Watch only ever observes terminal values here —
    // waiters are resumed between loop turns, never mid-stack.
    publish_processing(true);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (state_ != TlsCertificateWatcherState::Running) {
            break;
        }
        const TlsCertificateVersionState version_state = store_->version_state();
        const bool quic_enabled = store_->quic_enabled();
        const bool prepare_bootstrap = !store_->bootstrap_identity();
        const auto started = std::chrono::steady_clock::now();
        CompiledTlsCertificateConfigResult result =
                compiler_->compile_tls(data->content, version_state, quic_enabled, prepare_bootstrap, force_compile);
        const auto duration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
        if (duration > kTlsCompileWarnThreshold) {
            LOG(LOG_CONFIG, WARN) << "tls_compile_slow"
                                  << " duration_ms="
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        }
        if (result && result->compilation_skipped && result->version &&
            !store_->classify(*result->version, result->content_digest)) {
            // Skipped snapshot that is not confirmably the loaded one;
            // recompile once with the skip bypassed. The classify outcome is
            // recomputed by apply_result for the status mapping.
            force_compile = true;
            continue;
        }
        apply_result(data, std::move(result));
        break;
    }
    publish_processing(false);
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
    LOG(LOG_CONFIG, WARN) << "tls_config_failed" << " md5=\"" << md5 << "\""
                          << " code=" << error_code_name(error.code) << " field=\"" << error.field << "\""
                          << " offset=" << error.offset << " error=\"" << error.message << "\"";
    last_failure_ = TlsCertificateWatcherFailure{
            .stage = "compile",
            .code = std::string(error_code_name(error.code)),
            .md5 = std::move(md5),
            .error = std::move(error),
            .observed_at_unix_millis = access_activation_unix_millis(*loop_),
    };
    const auto readiness = readiness_.current();
    if (!readiness.value || *readiness.value == TlsCertificateReadiness::Awaiting) {
        initial_rejected_ = true;
        readiness_publisher_->publish(TlsCertificateReadiness::Failed);
    }
    publish_evidence();
}

void TlsCertificateWatcher::apply_result(const std::shared_ptr<const nacos::ConfigData> &data,
                                         CompiledTlsCertificateConfigResult result) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(data);
    if (!result) {
        report_failure(std::string(data->md5), std::move(result.error()));
        return;
    }
    if (!result->version) {
        candidate_status_ = AccessActivationCandidateStatus::Accepted;
        publish_evidence();
        return;
    }

    if (result->compilation_skipped) {
        // Reached only when the skipped snapshot is confirmably the loaded
        // one; the no-longer-loaded replay is handled by
        // compile_tls_inline's bounded re-pass.
        TlsCertificateClassification classified = store_->classify(*result->version, result->content_digest);
        FIBER_ASSERT(classified);
        if (!*classified) {
            report_failure(std::string(data->md5), std::move(classified->error()));
            return;
        }
        if (**classified == TlsCertificateUpdateStatus::Published) {
            ++successful_updates_;
            active_md5_ = std::string(data->md5);
            active_at_unix_millis_ = access_activation_unix_millis(*loop_);
            readiness_publisher_->publish(TlsCertificateReadiness::Ready);
        } else if (**classified == TlsCertificateUpdateStatus::VersionUnchanged) {
            active_md5_ = std::string(data->md5);
            active_at_unix_millis_ = access_activation_unix_millis(*loop_);
        }
        candidate_status_ = AccessActivationCandidateStatus::Accepted;
        publish_evidence();
        return;
    }
    if (!result->prepared) {
        report_failure(std::string(data->md5), TlsCertificateConfigError{
                                                       .code = TlsCertificateConfigErrorCode::InvalidField,
                                                       .field = "compiler",
                                                       .message = "TLS compiler returned no prepared snapshot",
                                               });
        return;
    }
    auto updated = store_->commit(std::move(*result->prepared));
    if (!updated) {
        report_failure(std::string(data->md5), std::move(updated.error()));
        return;
    }
    if (*updated == TlsCertificateUpdateStatus::Published) {
        ++successful_updates_;
        active_md5_ = std::string(data->md5);
        active_at_unix_millis_ = access_activation_unix_millis(*loop_);
        readiness_publisher_->publish(TlsCertificateReadiness::Ready);
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

} // namespace fiber::access_server
