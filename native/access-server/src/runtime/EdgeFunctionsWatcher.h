#ifndef FIBER_ACCESS_SERVER_EDGE_FUNCTIONS_WATCHER_H
#define FIBER_ACCESS_SERVER_EDGE_FUNCTIONS_WATCHER_H

#include "../config/AccessConfig.h"
#include "../config/AccessConfigLimits.h"
#include "../observability/AccessActivationEvidence.h"
#include "../observability/AccessConfigMetrics.h"
#include "../routing/EdgeFunctionLibrary.h"
#include "../routing/EdgeFunctionsSnapshot.h"
#include "RouteConfigStore.h"
#include "SubscriptionLifecycle.h"

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>

namespace fiber::access_server {

inline constexpr std::string_view kEdgeFunctionIndexDataId = "ploto.unified-access.edge-functions";

enum class EdgeFunctionsWatcherState : std::uint8_t {
    Created,
    Running,
    Failed,
    Stopping,
    Stopped,
};

struct EdgeFunctionsWatcherOptions {
    std::string index_data_id = std::string(kEdgeFunctionIndexDataId);
    std::string group = std::string(kProjectRouteGroup);
};

struct EdgeFunctionsWatcherFailure {
    std::string stage;
    std::string code;
    std::string detail;
    std::int64_t observed_at_unix_millis = 0;
};

// Owns the two-level edge-function subscription graph: the JSON index plus one
// script Data ID per referenced function. Assembly is all-or-nothing
// (docs/edge-functions-and-waf-design.md §3.1/§3.6): a snapshot is published
// only when every referenced script arrived and its sha256 matches the index;
// otherwise the previously published set stays active. Compilations are
// content-addressed by the verified script hash, so unchanged functions are
// never recompiled across index updates.
//
// start(), shutdown(), and all subscription mutations are owner-loop-only. A
// failed script subscription marks the whole set unavailable (requests fail
// open); there is no per-script retry in v1 — the index or script republishing
// re-drives assembly.
class EdgeFunctionsWatcher final : public common::NonCopyable, public common::NonMovable {
public:
    EdgeFunctionsWatcher(event::EventLoop &loop, nacos::ConfigService &config_service, RouteConfigStore &store,
                         EdgeFunctionsWatcherOptions options = {}, AccessConfigMetricsObserver metrics = {},
                         AccessEdgeFunctionsActivationEvidenceObserver observer = {});
    ~EdgeFunctionsWatcher() noexcept;

    [[nodiscard]] std::expected<void, nacos::ConfigServiceError> start();
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_readiness() { return readiness_.subscribe(); }

    [[nodiscard]] EdgeFunctionsWatcherState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t successful_updates() const noexcept { return successful_updates_; }
    [[nodiscard]] std::uint64_t failed_updates() const noexcept { return failed_updates_; }
    [[nodiscard]] const std::optional<EdgeFunctionsWatcherFailure> &last_failure() const noexcept {
        return last_failure_;
    }
    [[nodiscard]] std::size_t script_subscription_count() const noexcept { return scripts_.size(); }
    [[nodiscard]] std::size_t compiled_cache_size() const noexcept { return compile_cache_.size(); }

private:
    struct ScriptEntry {
        ScriptEntry(event::EventLoop &loop, EdgeFunctionsWatcher *owner) noexcept :
            subscription(loop), watcher(owner) {}

        std::string id;
        std::string sha256_hex;
        SubscriptionLifecycle subscription;
        // Set once a payload arrived and matched the index sha256.
        std::optional<std::string> verified_content;
        // Subscription callback context; the entry is heap-owned and fully
        // constructed before subscribe() so early pushes see valid state.
        EdgeFunctionsWatcher *watcher = nullptr;
    };

    static void index_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;
    static void script_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept;

    void apply_index(const nacos::ConfigData &data);
    void reconcile_scripts(const EdgeFunctionIndexConfig &index);
    void apply_script(ScriptEntry &entry, const nacos::ConfigData &data);
    // Publishes a new EdgeFunctionsState when every referenced script is
    // verified and compiles; returns true when the active set changed.
    bool try_assemble();
    void report_failure(std::string_view stage, std::string_view code, std::string detail);
    void emit_event(AccessConfigMetricEvent event) noexcept;
    void publish_evidence() const noexcept;
    void mark_active(std::size_t function_count, std::size_t script_bytes) noexcept;
    [[nodiscard]] std::shared_ptr<EdgeFunctionProgram> compile_cached(const std::string &sha256_hex,
                                                                      std::string_view source, bool &compiled);

    async::Watch<bool> readiness_{false};
    std::optional<async::Watch<bool>::Publisher> readiness_publisher_;
    event::EventLoop *loop_ = nullptr;
    nacos::ConfigService *config_service_ = nullptr;
    RouteConfigStore *store_ = nullptr;
    EdgeFunctionsWatcherOptions options_;
    SubscriptionLifecycle index_subscription_;
    // Script data_id -> entry; mirrors the current index references.
    std::map<std::string, std::unique_ptr<ScriptEntry>> scripts_;
    // sha256 -> compiled program; bounded by 2x the index function limit with
    // unreferenced entries evicted first on overflow.
    std::map<std::string, std::shared_ptr<EdgeFunctionProgram>> compile_cache_;
    // The parsed index whose references are currently subscribed.
    std::optional<EdgeFunctionIndexConfig> desired_index_;
    std::optional<EdgeFunctionsWatcherFailure> last_failure_;
    EdgeFunctionsWatcherState state_ = EdgeFunctionsWatcherState::Created;
    std::uint64_t generation_ = 0;
    std::uint64_t successful_updates_ = 0;
    std::uint64_t failed_updates_ = 0;
    AccessConfigMetricsObserver metrics_;
    AccessEdgeFunctionsActivationEvidenceObserver observer_;
    AccessActivationCandidateStatus candidate_status_ = AccessActivationCandidateStatus::Awaiting;
    std::string observed_index_md5_;
    std::string active_index_md5_;
    std::string observed_index_sha256_;
    std::string active_index_sha256_;
    std::int64_t observed_at_unix_millis_ = 0;
    std::int64_t active_at_unix_millis_ = 0;
    std::size_t active_function_count_ = 0;
    std::size_t active_script_bytes_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_EDGE_FUNCTIONS_WATCHER_H
