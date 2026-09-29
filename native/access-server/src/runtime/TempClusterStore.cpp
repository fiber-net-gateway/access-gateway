#include "TempClusterStore.h"

#include "../config/AccessConfigErrorBuilder.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

void sort_entries(std::vector<TempClusterConfigEntry> &entries) {
    std::sort(entries.begin(), entries.end(),
              [](const TempClusterConfigEntry &left, const TempClusterConfigEntry &right) {
                  return left.service < right.service;
              });
    for (TempClusterConfigEntry &entry: entries) {
        std::sort(entry.clusters.begin(), entry.clusters.end());
    }
}

bool unconditional_fallback(void *, std::string_view, std::string_view, std::string_view,
                            ProxyAddressSelectErrorCode) noexcept {
    return true;
}

} // namespace

TempClusterStore::TempClusterStore() : published_(std::make_shared<const Snapshot>()) {}

std::expected<TempClusterUpdateStatus, AccessConfigError>
TempClusterStore::apply(const std::optional<TempClusterConfig> &config) {
    if (!config) {
        return std::unexpected(config_detail::make_error(AccessConfigErrorCode::InvalidField, "payload",
                                                         "temp-cluster configuration must decode to a value"));
    }
    FIBER_ASSERT(next_generation_ != std::numeric_limits<std::uint64_t>::max());
    auto candidate = std::make_shared<Snapshot>();
    candidate->generation = next_generation_ + 1;
    candidate->entries = *config;
    sort_entries(candidate->entries);

    std::shared_ptr<const Snapshot> published = std::move(candidate);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(published), std::memory_order_release);
#else
    std::atomic_store_explicit(&published_, std::move(published), std::memory_order_release);
#endif
    ++next_generation_;
    return TempClusterUpdateStatus::Published;
}

bool TempClusterStore::contains(std::string_view service, std::string_view cluster) const noexcept {
    const std::vector<TempClusterConfigEntry> &entries = pin()->entries;
    const auto iterator = std::lower_bound(
            entries.begin(), entries.end(), service,
            [](const TempClusterConfigEntry &entry, std::string_view value) { return entry.service < value; });
    if (iterator == entries.end() || iterator->service != service) {
        return false;
    }
    const std::vector<std::string> &clusters = iterator->clusters;
    return std::binary_search(clusters.begin(), clusters.end(), cluster);
}

ProxyClusterFallback TempClusterStore::adapter() noexcept {
    return ProxyClusterFallback{
            .context = this,
            .should_fallback = &TempClusterStore::should_fallback_request,
    };
}

bool TempClusterStore::should_fallback_request(void *context, std::string_view service, std::string_view,
                                               std::string_view requested_cluster,
                                               ProxyAddressSelectErrorCode error) noexcept {
    if (error != ProxyAddressSelectErrorCode::NoHosts) {
        return false;
    }
    return !static_cast<TempClusterStore *>(context)->contains(service, requested_cluster);
}

std::size_t TempClusterStore::service_count() const noexcept { return pin()->entries.size(); }

std::uint64_t TempClusterStore::generation() const noexcept { return pin()->generation; }

std::shared_ptr<const TempClusterStore::Snapshot> TempClusterStore::pin() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return published_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&published_, std::memory_order_acquire);
#endif
}

ProxyClusterFallback make_unconditional_cluster_fallback() noexcept {
    return ProxyClusterFallback{
            .context = nullptr,
            .should_fallback = &unconditional_fallback,
    };
}

} // namespace fiber::access_server
