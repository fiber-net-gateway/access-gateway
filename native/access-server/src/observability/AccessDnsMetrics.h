#ifndef FIBER_ACCESS_SERVER_ACCESS_DNS_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_DNS_METRICS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/dns/DnsResolverConfig.h>

namespace fiber::access_server {

enum class AccessDnsConfigSource : std::uint8_t {
    System,
    Override,
    Count,
};

enum class AccessDnsResolverState : std::uint8_t {
    Stopped,
    Starting,
    Ready,
    Failed,
    Stopping,
    Count,
};

struct AccessDnsMetricsStatus {
    AccessDnsConfigSource source = AccessDnsConfigSource::System;
    AccessDnsResolverState state = AccessDnsResolverState::Stopped;
    dns::ResolverUnsupportedFeature unsupported = dns::ResolverUnsupportedFeature::None;
    std::uint64_t configured_nameservers = 0;
    std::uint64_t active_resolvers = 0;
    std::uint64_t initialization_successes = 0;
    std::uint64_t initialization_failures = 0;
};

struct AccessDnsMetricsObserver {
    using ConfigureFunction = void (*)(void *context, AccessDnsConfigSource source, std::size_t nameservers,
                                       dns::ResolverUnsupportedFeature unsupported) noexcept;
    using StateFunction = void (*)(void *context, AccessDnsResolverState state, std::size_t active_resolvers) noexcept;
    using InitializationFunction = void (*)(void *context, bool success) noexcept;

    void configure(AccessDnsConfigSource source, std::size_t nameservers,
                   dns::ResolverUnsupportedFeature unsupported) const noexcept {
        if (on_configure != nullptr) {
            on_configure(context, source, nameservers, unsupported);
        }
    }

    void set_state(AccessDnsResolverState state, std::size_t active_resolvers) const noexcept {
        if (on_state != nullptr) {
            on_state(context, state, active_resolvers);
        }
    }

    void initialized(bool success) const noexcept {
        if (on_initialization != nullptr) {
            on_initialization(context, success);
        }
    }

    void *context = nullptr;
    ConfigureFunction on_configure = nullptr;
    StateFunction on_state = nullptr;
    InitializationFunction on_initialization = nullptr;
};

// DNS configuration is immutable after runtime construction. Lifecycle writes
// happen on the data-plane control loop and scrapes may run on another loop, so
// the bounded snapshot is represented only by independent atomics.
class AccessDnsMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] AccessDnsMetricsObserver observer() noexcept;
    [[nodiscard]] AccessDnsMetricsStatus status() const noexcept;
    void append_prometheus(std::string &output) const;

private:
    static void observe_configure(void *context, AccessDnsConfigSource source, std::size_t nameservers,
                                  dns::ResolverUnsupportedFeature unsupported) noexcept;
    static void observe_state(void *context, AccessDnsResolverState state, std::size_t active_resolvers) noexcept;
    static void observe_initialization(void *context, bool success) noexcept;

    std::atomic<std::uint8_t> source_{static_cast<std::uint8_t>(AccessDnsConfigSource::System)};
    std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(AccessDnsResolverState::Stopped)};
    std::atomic<std::uint32_t> unsupported_{0};
    std::atomic<std::uint64_t> configured_nameservers_{0};
    std::atomic<std::uint64_t> active_resolvers_{0};
    std::atomic<std::uint64_t> initialization_successes_{0};
    std::atomic<std::uint64_t> initialization_failures_{0};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_DNS_METRICS_H
