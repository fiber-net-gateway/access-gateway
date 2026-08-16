#ifndef FIBER_ACCESS_SERVER_ACCESS_ACTIVATION_EVIDENCE_H
#define FIBER_ACCESS_SERVER_ACCESS_ACTIVATION_EVIDENCE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::event {
class EventLoop;
}

namespace fiber::access_server {

inline constexpr std::uint64_t kAccessActivationEvidenceContractVersion = 1;
inline constexpr std::size_t kAccessActivationEvidenceDefaultPageSize = 100;
inline constexpr std::size_t kAccessActivationEvidenceMaxPageSize = 256;

enum class AccessActivationCandidateStatus : std::uint8_t {
    Awaiting,
    Processing,
    ReadyToPublish,
    Accepted,
    Rejected,
};

struct AccessActivationFailure {
    std::string stage;
    std::string code;
    std::string field;
    std::size_t offset = 0;
    std::int64_t observed_at_unix_millis = 0;
};

struct AccessActivationResourceEvidence {
    std::string data_id;
    std::string group;
    AccessActivationCandidateStatus candidate_status = AccessActivationCandidateStatus::Awaiting;
    std::string observed_md5;
    std::string active_md5;
    std::int64_t observed_at_unix_millis = 0;
    std::int64_t active_at_unix_millis = 0;
    std::optional<AccessActivationFailure> failure;
};

struct AccessActivationProjectEvidence {
    std::string name;
    std::string data_id;
    std::string group;
    std::string subscription_state;
    AccessActivationCandidateStatus candidate_status = AccessActivationCandidateStatus::Awaiting;
    std::string observed_md5;
    std::optional<std::int32_t> observed_version;
    std::string active_md5;
    std::optional<std::int32_t> active_version;
    std::uint64_t active_snapshot_generation = 0;
    bool active_loaded = false;
    std::int64_t observed_at_unix_millis = 0;
    std::int64_t active_at_unix_millis = 0;
    std::optional<AccessActivationFailure> failure;
};

struct AccessRouteActivationEvidence {
    std::string watcher_state = "created";
    std::string readiness_state = "waiting_for_project_list";
    AccessActivationResourceEvidence project_list;
    std::uint64_t snapshot_generation = 0;
    std::int64_t snapshot_published_at_unix_millis = 0;
    std::vector<AccessActivationProjectEvidence> projects;
};

struct AccessGrayActivationEvidence {
    std::string watcher_state = "created";
    AccessActivationResourceEvidence resource;
    std::uint64_t generation = 0;
    std::size_t rule_count = 0;
};

struct AccessTlsActivationEvidence {
    bool enabled = false;
    std::string watcher_state = "created";
    AccessActivationResourceEvidence resource;
    std::uint64_t version = 0;
    std::size_t certificate_count = 0;
};

struct AccessActivationEvidenceIdentity {
    std::string instance_id;
    std::string build_version;
    std::string build_revision;
    std::int64_t started_at_unix_millis = 0;
};

struct AccessActivationEvidenceSnapshot {
    std::uint64_t revision = 0;
    AccessActivationEvidenceIdentity identity;
    AccessRouteActivationEvidence route;
    AccessGrayActivationEvidence gray;
    AccessTlsActivationEvidence tls;
    std::string route_snapshot_fingerprint_sha256;
};

struct AccessRouteActivationEvidenceObserver {
    using Function = void (*)(void *context, const AccessRouteActivationEvidence &evidence) noexcept;

    void *context = nullptr;
    Function on_update = nullptr;
};

struct AccessGrayActivationEvidenceObserver {
    using Function = void (*)(void *context, const AccessGrayActivationEvidence &evidence) noexcept;

    void *context = nullptr;
    Function on_update = nullptr;
};

struct AccessTlsActivationEvidenceObserver {
    using Function = void (*)(void *context, const AccessTlsActivationEvidence &evidence) noexcept;

    void *context = nullptr;
    Function on_update = nullptr;
};

// Configuration watchers are single-writer on the Nacos EventLoop. Status
// workers only pin complete immutable evidence snapshots.
class AccessActivationEvidenceStore final : public common::NonCopyable, public common::NonMovable {
public:
    AccessActivationEvidenceStore(event::EventLoop &owner, AccessActivationEvidenceIdentity identity);

    [[nodiscard]] AccessRouteActivationEvidenceObserver route_observer() noexcept;
    [[nodiscard]] AccessGrayActivationEvidenceObserver gray_observer() noexcept;
    [[nodiscard]] AccessTlsActivationEvidenceObserver tls_observer() noexcept;

    [[nodiscard]] std::shared_ptr<const AccessActivationEvidenceSnapshot> pin() const noexcept {
        return published_.load(std::memory_order_acquire);
    }

private:
    static void observe_route(void *context, const AccessRouteActivationEvidence &evidence) noexcept;
    static void observe_gray(void *context, const AccessGrayActivationEvidence &evidence) noexcept;
    static void observe_tls(void *context, const AccessTlsActivationEvidence &evidence) noexcept;

    void update_route(const AccessRouteActivationEvidence &evidence) noexcept;
    void update_gray(const AccessGrayActivationEvidence &evidence) noexcept;
    void update_tls(const AccessTlsActivationEvidence &evidence) noexcept;
    void publish() noexcept;
    [[nodiscard]] std::string route_fingerprint() const noexcept;

    event::EventLoop *owner_ = nullptr;
    AccessActivationEvidenceIdentity identity_;
    AccessRouteActivationEvidence route_;
    AccessGrayActivationEvidence gray_;
    AccessTlsActivationEvidence tls_;
    std::uint64_t revision_ = 0;
    std::atomic<std::shared_ptr<const AccessActivationEvidenceSnapshot>> published_;
};

[[nodiscard]] std::string_view access_activation_candidate_status_name(AccessActivationCandidateStatus status) noexcept;
[[nodiscard]] std::int64_t access_activation_unix_millis(const event::EventLoop &loop) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_ACTIVATION_EVIDENCE_H
