#include "AccessActivationEvidence.h"

#include <array>
#include <chrono>
#include <limits>
#include <utility>

#include <openssl/sha.h>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

char hexadecimal(std::uint8_t value) noexcept {
    constexpr std::string_view digits = "0123456789abcdef";
    return digits[value & 0x0fU];
}

void append_field(std::string &input, std::string_view value) {
    input.append(std::to_string(value.size()));
    input.push_back(':');
    input.append(value);
    input.push_back(';');
}

} // namespace

std::int64_t access_activation_unix_millis(const event::EventLoop &loop) noexcept {
    using Milliseconds = std::chrono::milliseconds;
    static const std::int64_t epoch_offset =
            std::chrono::duration_cast<Milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() -
            std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return epoch_offset + std::chrono::duration_cast<Milliseconds>(loop.now().time_since_epoch()).count();
}

std::string_view access_activation_candidate_status_name(AccessActivationCandidateStatus status) noexcept {
    switch (status) {
        case AccessActivationCandidateStatus::Awaiting:
            return "awaiting";
        case AccessActivationCandidateStatus::Processing:
            return "processing";
        case AccessActivationCandidateStatus::ReadyToPublish:
            return "ready_to_publish";
        case AccessActivationCandidateStatus::Accepted:
            return "accepted";
        case AccessActivationCandidateStatus::Rejected:
            return "rejected";
    }
    return "awaiting";
}

AccessActivationEvidenceStore::AccessActivationEvidenceStore(event::EventLoop &owner,
                                                             AccessActivationEvidenceIdentity identity) :
    owner_(&owner), identity_(std::move(identity)) {
    auto initial = std::make_shared<AccessActivationEvidenceSnapshot>();
    revision_ = 1;
    initial->revision = revision_;
    initial->identity = identity_;
    initial->route = route_;
    initial->gray = gray_;
    initial->tls = tls_;
    initial->route_snapshot_fingerprint_sha256 = route_fingerprint();
    published_.store(std::move(initial), std::memory_order_relaxed);
}

AccessRouteActivationEvidenceObserver AccessActivationEvidenceStore::route_observer() noexcept {
    return AccessRouteActivationEvidenceObserver{
            .context = this,
            .on_update = &observe_route,
    };
}

AccessGrayActivationEvidenceObserver AccessActivationEvidenceStore::gray_observer() noexcept {
    return AccessGrayActivationEvidenceObserver{
            .context = this,
            .on_update = &observe_gray,
    };
}

AccessTlsActivationEvidenceObserver AccessActivationEvidenceStore::tls_observer() noexcept {
    return AccessTlsActivationEvidenceObserver{
            .context = this,
            .on_update = &observe_tls,
    };
}

void AccessActivationEvidenceStore::observe_route(void *context,
                                                  const AccessRouteActivationEvidence &evidence) noexcept {
    static_cast<AccessActivationEvidenceStore *>(context)->update_route(evidence);
}

void AccessActivationEvidenceStore::observe_gray(void *context, const AccessGrayActivationEvidence &evidence) noexcept {
    static_cast<AccessActivationEvidenceStore *>(context)->update_gray(evidence);
}

void AccessActivationEvidenceStore::observe_tls(void *context, const AccessTlsActivationEvidence &evidence) noexcept {
    static_cast<AccessActivationEvidenceStore *>(context)->update_tls(evidence);
}

void AccessActivationEvidenceStore::update_route(const AccessRouteActivationEvidence &evidence) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    route_ = evidence;
    publish();
}

void AccessActivationEvidenceStore::update_gray(const AccessGrayActivationEvidence &evidence) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    gray_ = evidence;
    publish();
}

void AccessActivationEvidenceStore::update_tls(const AccessTlsActivationEvidence &evidence) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    tls_ = evidence;
    publish();
}

void AccessActivationEvidenceStore::publish() noexcept {
    FIBER_ASSERT(owner_->in_loop());
    FIBER_ASSERT(revision_ != std::numeric_limits<std::uint64_t>::max());
    auto snapshot = std::make_shared<AccessActivationEvidenceSnapshot>();
    snapshot->revision = ++revision_;
    snapshot->identity = identity_;
    snapshot->route = route_;
    snapshot->gray = gray_;
    snapshot->tls = tls_;
    snapshot->route_snapshot_fingerprint_sha256 = route_fingerprint();
    published_.store(std::move(snapshot), std::memory_order_release);
}

std::string AccessActivationEvidenceStore::route_fingerprint() const noexcept {
    std::string input;
    input.reserve(route_.projects.size() * 96U + 64U);
    append_field(input, route_.project_list.active_md5);
    input.append(std::to_string(route_.snapshot_generation));
    input.push_back(';');
    for (const AccessActivationProjectEvidence &project: route_.projects) {
        append_field(input, project.name);
        append_field(input, project.active_md5);
        input.append(project.active_version ? std::to_string(*project.active_version) : "null");
        input.push_back(';');
        input.push_back(project.active_loaded ? '1' : '0');
        input.push_back(';');
    }

    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const std::uint8_t *>(input.data()), input.size(), digest.data());
    std::string output(digest.size() * 2U, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        output[index * 2U] = hexadecimal(static_cast<std::uint8_t>(digest[index] >> 4U));
        output[index * 2U + 1U] = hexadecimal(digest[index]);
    }
    return output;
}

} // namespace fiber::access_server
