#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVICE_DISCOVERY_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVICE_DISCOVERY_H

#include "../config/AccessConfig.h"
#include "../observability/AccessDiscoveryMetrics.h"
#include "../routing/ProxyAddressSelector.h"
#include "SmoothWeightedRoundRobin.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/nacos/discovery/ServiceDiscovery.h>

namespace fiber::access_server {

class AccessServiceState final : public common::NonCopyable, public common::NonMovable {
public:
    using Selection = AccessUpstreamSwrr::Selection;

    AccessServiceState() noexcept;
    ~AccessServiceState() noexcept;

    // Lifecycle callbacks run only on ServiceDiscovery's owner EventLoop.
    // select() is the sole cross-loop operation and reads a published immutable
    // cluster directory before entering the selected balancer.
    void initialize(AccessUpstreamSwrr::Options options, std::string_view zone,
                    AccessDiscoveryMetricsObserver metrics_observer = {}) noexcept;
    void update(const nacos::ServiceInfo &snapshot) noexcept;
    void retire(nacos::ServiceRetireReason reason) noexcept;

    [[nodiscard]] std::expected<Selection, SwrrSelectError>
    select(std::string_view cluster, std::span<const std::uint64_t> excluded_selection_tokens) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct AccessServiceOps {
    using State = AccessServiceState;

    void on_init(const nacos::ServiceKeyView &key, State &state) noexcept;
    void on_update(const nacos::ServiceKeyView &key, State &state,
                   const std::shared_ptr<const nacos::ServiceInfo> &snapshot) noexcept;
    void on_retire(const nacos::ServiceKeyView &key, State &state, nacos::ServiceRetireReason reason) noexcept;

    AccessUpstreamSwrr::Options swrr_options{};
    std::string zone;
    AccessDiscoveryMetricsObserver metrics_observer;
};

using AccessServiceDiscovery = nacos::ServiceDiscovery<AccessServiceOps>;

struct AccessServiceDiscoveryOptions {
    std::string group = std::string(kDefaultNacosGroup);
    std::string zone;
    AccessUpstreamSwrr::Options swrr_options{};
};

// Synchronous owner-loop adapter used while binding one compiled project
// snapshot. Each service route acquires its own Lease; ServiceDiscovery
// performs key-level subscription deduplication.
class AccessServiceSelectorFactory final : public common::NonCopyable, public common::NonMovable {
public:
    AccessServiceSelectorFactory() noexcept = default;
    AccessServiceSelectorFactory(AccessServiceDiscovery &discovery, AccessServiceDiscoveryOptions options,
                                 AccessDiscoveryMetricsObserver metrics_observer = {}) noexcept;

    [[nodiscard]] ProxyAddressSelectorFactory adapter() noexcept;

private:
    [[nodiscard]] static ProxyAddressSelectorFactory::Result create_address_selector(void *context, std::string service,
                                                                                     std::string cluster);

    AccessServiceDiscovery *discovery_ = nullptr;
    AccessServiceDiscoveryOptions options_;
    AccessDiscoveryMetricsObserver metrics_observer_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVICE_DISCOVERY_H
