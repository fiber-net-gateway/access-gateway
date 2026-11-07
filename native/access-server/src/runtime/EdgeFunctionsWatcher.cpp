#include "EdgeFunctionsWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include <fiber/common/Assert.h>
#include <openssl/sha.h>

namespace fiber::access_server {
namespace {

void encode_hex(const unsigned char *digest, std::size_t size, std::string &out) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.clear();
    out.reserve(size * 2U);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[digest[i] >> 4U]);
        out.push_back(kHex[digest[i] & 0x0FU]);
    }
}

[[nodiscard]] std::string sha256_hex(std::string_view content) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    (void) SHA256(reinterpret_cast<const unsigned char *>(content.data()), content.size(), digest.data());
    std::string hex;
    encode_hex(digest.data(), digest.size(), hex);
    return hex;
}

std::string_view watcher_state_name(EdgeFunctionsWatcherState state) noexcept {
    switch (state) {
        case EdgeFunctionsWatcherState::Created:
            return "created";
        case EdgeFunctionsWatcherState::Running:
            return "running";
        case EdgeFunctionsWatcherState::Failed:
            return "failed";
        case EdgeFunctionsWatcherState::Stopping:
            return "stopping";
        case EdgeFunctionsWatcherState::Stopped:
            return "stopped";
    }
    return "created";
}

} // namespace

EdgeFunctionsWatcher::EdgeFunctionsWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                           RouteConfigStore &store, EdgeFunctionsWatcherOptions options,
                                           AccessConfigMetricsObserver metrics,
                                           AccessEdgeFunctionsActivationEvidenceObserver observer) :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)),
    index_subscription_(loop), metrics_(metrics), observer_(observer) {
    readiness_publisher_ = readiness_.acquire_publisher();
    FIBER_ASSERT(readiness_publisher_);
}

EdgeFunctionsWatcher::~EdgeFunctionsWatcher() noexcept {
    FIBER_ASSERT(state_ == EdgeFunctionsWatcherState::Created || state_ == EdgeFunctionsWatcherState::Stopped);
}

std::expected<void, nacos::ConfigServiceError> EdgeFunctionsWatcher::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != EdgeFunctionsWatcherState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "edge-function watcher is already started",
        });
    }
    state_ = EdgeFunctionsWatcherState::Running;
    auto subscribed = index_subscription_.subscribe(*config_service_, options_.index_data_id, options_.group,
                                                    &index_notify, this);
    if (!subscribed) {
        state_ = EdgeFunctionsWatcherState::Created;
        index_subscription_.reset_start_failure();
        return std::unexpected(std::move(subscribed.error()));
    }
    publish_evidence();
    return {};
}

async::Task<void> EdgeFunctionsWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == EdgeFunctionsWatcherState::Stopped) {
        co_return;
    }
    if (state_ == EdgeFunctionsWatcherState::Running) {
        state_ = EdgeFunctionsWatcherState::Stopping;
    }
    readiness_publisher_->publish(false);
    index_subscription_.stop();
    for (auto &[data_id, entry]: scripts_) {
        (void) data_id;
        entry->subscription.stop();
    }
    scripts_.clear();
    desired_index_.reset();
    state_ = EdgeFunctionsWatcherState::Stopped;
    publish_evidence();
    co_return;
}

void EdgeFunctionsWatcher::index_notify(void *context,
                                        const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<EdgeFunctionsWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        if (owner.state_ == EdgeFunctionsWatcherState::Running) {
            owner.index_subscription_.fail(nacos::ConfigServiceError{
                    .code = nacos::ConfigServiceErrorCode::Shutdown,
                    .io_error = common::IoErr::NotConnected,
                    .message = "edge-function index subscription closed before shutdown",
            });
            owner.state_ = EdgeFunctionsWatcherState::Failed;
            owner.readiness_publisher_->publish(false);
            ++owner.failed_updates_;
            owner.report_failure("subscription", "index_closed", "edge-function index subscription closed");
            owner.emit_event(AccessConfigMetricEvent::EdgeFunctionIndexSubscriptionFailed);
            owner.publish_evidence();
        }
        return;
    }
    if (result.data && owner.state_ == EdgeFunctionsWatcherState::Running) {
        (void) owner.index_subscription_.observe_value();
        owner.apply_index(*result.data);
    }
}

void EdgeFunctionsWatcher::script_notify(void *context,
                                         const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    // The context is the entry itself (map nodes are address-stable and the
    // entry outlives its subscription).
    ScriptEntry &entry = *static_cast<ScriptEntry *>(context);
    EdgeFunctionsWatcher &owner = *entry.watcher;
    if (result.kind == nacos::ResultKind::Closed) {
        // Per-script closure without shutdown: the set stays incomplete; the
        // active snapshot (if any) is retained and readiness drops.
        if (owner.state_ == EdgeFunctionsWatcherState::Running) {
            owner.readiness_publisher_->publish(false);
            ++owner.failed_updates_;
            owner.report_failure("subscription", "script_closed", entry.name);
            owner.emit_event(AccessConfigMetricEvent::EdgeFunctionScriptSubscriptionFailed);
            owner.publish_evidence();
        }
        return;
    }
    if (!result.data || owner.state_ != EdgeFunctionsWatcherState::Running) {
        return;
    }
    (void) entry.subscription.observe_value();
    owner.apply_script(entry, *result.data);
}

void EdgeFunctionsWatcher::apply_index(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    const std::string_view content =
            data.state == nacos::ConfigState::NotFound ? std::string_view{} : std::string_view(data.content);
    const AccessActivationCandidateStatus previous_status = candidate_status_;
    observed_index_md5_ = std::string(data.md5);
    observed_index_sha256_ = sha256_hex(content);
    observed_at_unix_millis_ = access_activation_unix_millis(*loop_);
    candidate_status_ = AccessActivationCandidateStatus::Processing;
    last_failure_.reset();
    auto parsed = parse_edge_function_index_config(content);
    if (!parsed) {
        ++failed_updates_;
        report_failure("decode", "invalid_index", parsed.error().message);
        emit_event(AccessConfigMetricEvent::EdgeFunctionIndexDecodeFailed);
        publish_evidence();
        return;
    }
    if (!parsed->has_value()) {
        // Empty content (including a NotFound push) is keep-current per the
        // codec contract. Operators unmount by publishing JSON null or an
        // empty functions array, which lands in the parsed-but-empty branch
        // of try_assemble().
        candidate_status_ = previous_status;
        publish_evidence();
        return;
    }
    desired_index_ = std::move(**parsed);
    reconcile_scripts(*desired_index_);
    (void) try_assemble();
}

void EdgeFunctionsWatcher::reconcile_scripts(const EdgeFunctionIndexConfig &index) {
    // Stop subscriptions no longer referenced, then subscribe new references.
    // Entries are keyed by function name; the script Data ID is derived from
    // it (the wire index never carries a dataId field).
    for (auto it = scripts_.begin(); it != scripts_.end();) {
        const bool referenced =
                std::any_of(index.functions.begin(), index.functions.end(),
                            [&it](const EdgeFunctionIndexEntry &entry) { return entry.name == it->first; });
        if (!referenced) {
            it->second->subscription.stop();
            it = scripts_.erase(it);
            continue;
        }
        ++it;
    }
    for (const EdgeFunctionIndexEntry &function: index.functions) {
        if (const auto existing = scripts_.find(function.name); existing != scripts_.end()) {
            existing->second->name = function.name;
            existing->second->sha256_hex = function.sha256_hex;
            if (existing->second->verified_content &&
                sha256_hex(*existing->second->verified_content) != existing->second->sha256_hex) {
                // The index now references different content for this Data ID.
                existing->second->verified_content.reset();
            }
            if (!existing->second->verified_content && existing->second->unverified_content &&
                sha256_hex(*existing->second->unverified_content) == existing->second->sha256_hex) {
                existing->second->verified_content = std::move(existing->second->unverified_content);
                existing->second->unverified_content.reset();
            }
            continue;
        }
        auto entry = std::make_unique<ScriptEntry>(*loop_, this);
        entry->name = function.name;
        entry->sha256_hex = function.sha256_hex;
        ScriptEntry *entry_ptr = entry.get();
        scripts_[function.name] = std::move(entry);
        const std::string data_id = edge_function_script_data_id(function.name);
        auto subscribed =
                entry_ptr->subscription.subscribe(*config_service_, data_id, options_.group, &script_notify, entry_ptr);
        if (!subscribed) {
            entry_ptr->subscription.reset_start_failure();
            entry_ptr->subscription.stop();
            // Drop the dead entry: the lifecycle is one-shot, so the next
            // index publication (even identical bytes) re-subscribes it.
            scripts_.erase(function.name);
            ++failed_updates_;
            report_failure("subscription", "script_subscribe_failed", data_id);
            emit_event(AccessConfigMetricEvent::EdgeFunctionScriptSubscriptionFailed);
            publish_evidence();
        }
    }
}

void EdgeFunctionsWatcher::apply_script(ScriptEntry &entry, const nacos::ConfigData &data) {
    entry.unverified_content.reset();
    if (data.state == nacos::ConfigState::NotFound) {
        entry.verified_content.reset();
        ++failed_updates_;
        report_failure("script", "script_missing", entry.name);
        readiness_publisher_->publish(false);
        emit_event(AccessConfigMetricEvent::EdgeFunctionScriptRejected);
        publish_evidence();
        return;
    }
    const std::string_view content(data.content);
    if (content.size() > kAccessConfigLimits.edge_functions.max_script_payload_bytes) {
        entry.verified_content.reset();
        ++failed_updates_;
        report_failure("script", "script_too_large", entry.name);
        readiness_publisher_->publish(false);
        emit_event(AccessConfigMetricEvent::EdgeFunctionScriptRejected);
        publish_evidence();
        return;
    }
    const std::string actual = sha256_hex(content);
    if (actual != entry.sha256_hex) {
        entry.verified_content.reset();
        entry.unverified_content = std::string(content);
        ++failed_updates_;
        report_failure("script", "sha256_mismatch", entry.name);
        readiness_publisher_->publish(false);
        emit_event(AccessConfigMetricEvent::EdgeFunctionScriptRejected);
        publish_evidence();
        return;
    }
    entry.verified_content = std::string(content);
    (void) try_assemble();
}

bool EdgeFunctionsWatcher::try_assemble() {
    if (!desired_index_ || desired_index_->functions.empty()) {
        // An explicitly empty index unmounts everything.
        if (desired_index_ && store_->pin()->edge_function_count() != 0) {
            store_->set_edge_functions(std::make_shared<EdgeFunctionsState>());
            mark_active(0, 0);
            ++successful_updates_;
            last_failure_.reset();
            emit_event(AccessConfigMetricEvent::EdgeFunctionSetRemoved);
        }
        readiness_publisher_->publish(true);
        candidate_status_ = AccessActivationCandidateStatus::Accepted;
        publish_evidence();
        return true;
    }
    if (scripts_.size() != desired_index_->functions.size()) {
        return false;
    }
    if (active_index_sha256_ == observed_index_sha256_ && active_function_count_ == desired_index_->functions.size()) {
        // Identical index bytes are already assembled and active (this also
        // covers the replay that assembled during reconcile_scripts' own
        // subscribe() calls); republication is a no-op, like same-md5
        // project routes.
        readiness_publisher_->publish(true);
        candidate_status_ = AccessActivationCandidateStatus::Accepted;
        publish_evidence();
        return false;
    }
    // Phase 1: every referenced script must have arrived and verified before
    // any compilation work is spent on the set.
    for (const EdgeFunctionIndexEntry &function: desired_index_->functions) {
        const auto found = scripts_.find(function.name);
        if (found == scripts_.end() || !found->second->verified_content) {
            return false; // all-or-nothing: wait for the rest
        }
    }
    EdgeFunctionsState state;
    state.index = *desired_index_;
    state.programs.reserve(desired_index_->functions.size());
    std::size_t total_script_bytes = 0;
    for (const EdgeFunctionIndexEntry &function: desired_index_->functions) {
        const auto found = scripts_.find(function.name);
        bool compiled = false;
        std::shared_ptr<EdgeFunctionProgram> program =
                compile_cached(function.sha256_hex, *found->second->verified_content, compiled, function.phase);
        if (!program) {
            ++failed_updates_;
            report_failure("compile", "script_compile_failed", function.name);
            readiness_publisher_->publish(false);
            emit_event(AccessConfigMetricEvent::EdgeFunctionCompileFailed);
            publish_evidence();
            return false;
        }
        total_script_bytes += found->second->verified_content->size();
        state.programs.push_back(std::move(program));
    }
    if (total_script_bytes > kAccessConfigLimits.edge_functions.max_total_script_bytes) {
        ++failed_updates_;
        report_failure("compile", "total_script_bytes_exceeded", "combined script payload exceeds the limit");
        readiness_publisher_->publish(false);
        emit_event(AccessConfigMetricEvent::EdgeFunctionCompileFailed);
        publish_evidence();
        return false;
    }
    state.total_script_bytes = total_script_bytes;
    store_->set_edge_functions(std::make_shared<EdgeFunctionsState>(std::move(state)));
    mark_active(desired_index_->functions.size(), total_script_bytes);
    ++successful_updates_;
    last_failure_.reset();
    readiness_publisher_->publish(true);
    candidate_status_ = AccessActivationCandidateStatus::Accepted;
    emit_event(AccessConfigMetricEvent::EdgeFunctionSetPublished);
    publish_evidence();
    return true;
}

void EdgeFunctionsWatcher::mark_active(std::size_t function_count, std::size_t script_bytes) noexcept {
    active_function_count_ = function_count;
    active_script_bytes_ = script_bytes;
    active_index_md5_ = observed_index_md5_;
    active_index_sha256_ = observed_index_sha256_;
    active_at_unix_millis_ = observed_at_unix_millis_;
}

std::shared_ptr<EdgeFunctionProgram> EdgeFunctionsWatcher::compile_cached(const std::string &sha256_hex_value,
                                                                          std::string_view source, bool &compiled,
                                                                          EdgeFunctionPhase phase) {
    // $req.body resolves only for request_body scripts, so the same bytes are
    // two different programs; phase joins the cache key.
    std::string cache_key = sha256_hex_value;
    cache_key += phase == EdgeFunctionPhase::RequestBody ? ":body" : ":head";
    if (const auto found = compile_cache_.find(cache_key); found != compile_cache_.end()) {
        compiled = false;
        return found->second;
    }
    EdgeFunctionLibrary library(kAccessConfigLimits);
    auto program = library.compile(source, phase);
    if (!program) {
        compiled = true;
        return nullptr;
    }
    auto shared = std::make_shared<EdgeFunctionProgram>(std::move(*program));
    compiled = true;
    compile_cache_[std::move(cache_key)] = shared;
    // Bound the cache: keep referenced hashes, drop the rest, then the oldest.
    const std::size_t cap = std::max<std::size_t>(kAccessConfigLimits.edge_functions.max_functions * 2U, 8U);
    if (compile_cache_.size() > cap) {
        for (auto it = compile_cache_.begin(); it != compile_cache_.end() && compile_cache_.size() > cap;) {
            const bool referenced =
                    desired_index_ && std::any_of(desired_index_->functions.begin(), desired_index_->functions.end(),
                                                  [&it](const EdgeFunctionIndexEntry &entry) {
                                                      return it->first.rfind(entry.sha256_hex, 0) == 0;
                                                  });
            if (!referenced) {
                it = compile_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return shared;
}

void EdgeFunctionsWatcher::report_failure(std::string_view stage, std::string_view code, std::string detail) {
    candidate_status_ = AccessActivationCandidateStatus::Rejected;
    last_failure_ = EdgeFunctionsWatcherFailure{
            .stage = std::string(stage),
            .code = std::string(code),
            .detail = std::move(detail),
            .observed_at_unix_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count(),
    };
}

void EdgeFunctionsWatcher::emit_event(AccessConfigMetricEvent event) noexcept {
    if (metrics_.on_event) {
        metrics_.on_event(metrics_.context, event);
    }
}

void EdgeFunctionsWatcher::publish_evidence() const noexcept {
    if (!observer_.on_update) {
        return;
    }
    AccessEdgeFunctionsActivationEvidence evidence{
            .watcher_state = std::string(watcher_state_name(state_)),
            .index =
                    AccessActivationResourceEvidence{
                            .data_id = options_.index_data_id,
                            .group = options_.group,
                            .candidate_status = candidate_status_,
                            .observed_md5 = observed_index_md5_,
                            .active_md5 = active_index_md5_,
                            .observed_at_unix_millis = observed_at_unix_millis_,
                            .active_at_unix_millis = active_at_unix_millis_,
                    },
            .generation = successful_updates_,
            .function_count = active_function_count_,
            .total_script_bytes = active_script_bytes_,
            .active_index_sha256 = active_index_sha256_,
    };
    if (last_failure_) {
        evidence.index.failure = AccessActivationFailure{
                .stage = last_failure_->stage,
                .code = last_failure_->code,
                .field = "edge_functions",
                .offset = 0,
                .observed_at_unix_millis = last_failure_->observed_at_unix_millis,
        };
    }
    observer_.on_update(observer_.context, evidence);
}

} // namespace fiber::access_server
