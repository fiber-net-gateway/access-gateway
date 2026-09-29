#ifndef FIBER_ACCESS_SERVER_TEMP_CLUSTER_STORE_H
#define FIBER_ACCESS_SERVER_TEMP_CLUSTER_STORE_H

#include "../config/AccessConfig.h"
#include "../config/AccessConfigError.h"
#include "../routing/ProxyAddressSelector.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

enum class TempClusterUpdateStatus : std::uint8_t {
    Published,
};

// Java ReqTraceSpecifiedClusterDecider's temp-cluster whitelist: services whose
// request-specified clusters must keep their no-instance failure instead of
// falling back to the configured cluster. Single-writer control-plane updates
// publish one immutable snapshot; the request path pins it only after a
// selection failure, so reads stay off the hot path and need no worker slots.
class TempClusterStore final : public common::NonCopyable, public common::NonMovable {
public:
    TempClusterStore();

    [[nodiscard]] std::expected<TempClusterUpdateStatus, AccessConfigError>
    apply(const std::optional<TempClusterConfig> &config);

    [[nodiscard]] bool contains(std::string_view service, std::string_view cluster) const noexcept;

    // Java test decider semantics: fall back only when the request-specified
    // cluster has no host (NoHosts, Java S_NO_IP) and the service has not
    // registered that cluster in the whitelist. Circuit-open selections keep
    // their failure.
    [[nodiscard]] ProxyClusterFallback adapter() noexcept;

    [[nodiscard]] std::size_t service_count() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    struct Snapshot {
        std::uint64_t generation = 0;
        // Sorted by service, clusters sorted, for binary-search lookups.
        std::vector<TempClusterConfigEntry> entries;
    };

    static bool should_fallback_request(void *context, std::string_view service, std::string_view configured_cluster,
                                        std::string_view requested_cluster, ProxyAddressSelectErrorCode error) noexcept;

    [[nodiscard]] std::shared_ptr<const Snapshot> pin() const noexcept;

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const Snapshot>> published_;
#else
    std::shared_ptr<const Snapshot> published_;
#endif
    std::uint64_t next_generation_ = 0;
};

// Java production decider semantics (GrayTagClusterDecider): fall back to the
// configured cluster whenever the request-specified cluster yields no host,
// including circuit-open selections.
[[nodiscard]] ProxyClusterFallback make_unconditional_cluster_fallback() noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TEMP_CLUSTER_STORE_H
