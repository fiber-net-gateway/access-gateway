#ifndef FIBER_ACCESS_SERVER_ACCESS_DISCOVERY_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_DISCOVERY_METRICS_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::event {
class EventLoop;
}

namespace fiber::access_server {

enum class AccessNacosComponent : std::uint8_t {
    Client,
    ConfigService,
    NamingService,
    Count,
};

// These states describe only the application-owned component lifecycle. They
// are deliberately not transport connection or reconnect states.
enum class AccessNacosLifecycleState : std::uint8_t {
    Created,
    Starting,
    Running,
    Failed,
    Stopping,
    Stopped,
    Count,
};

enum class AccessNacosTransportComponent : std::uint8_t {
    ConfigService,
    NamingService,
    Count,
};

enum class AccessNacosTransportPhase : std::uint8_t {
    Created,
    Connecting,
    Ready,
    ReconnectBackoff,
    Stopping,
    Stopped,
    Count,
};

enum class AccessNacosTransportFailure : std::uint8_t {
    None,
    AuthenticationUnavailable,
    Transport,
    GrpcStatus,
    Protocol,
    Server,
    Shutdown,
    Count,
};

struct AccessNacosSubscriptionStatus {
    std::uint64_t active = 0;
    std::uint64_t pending = 0;
    std::uint64_t registered = 0;
    std::uint64_t synchronized = 0;
};

struct AccessNacosRegistrationStatus {
    std::uint64_t active = 0;
    std::uint64_t pending = 0;
    std::uint64_t registered = 0;
};

// Application-owned copy of Fiber's bounded status schema. Keeping this type
// independent prevents the observability component from acquiring a Nacos
// runtime dependency while preserving every fixed-width field exactly.
struct AccessNacosTransportStatus {
    AccessNacosTransportPhase phase = AccessNacosTransportPhase::Created;
    AccessNacosTransportFailure failure = AccessNacosTransportFailure::None;
    bool rpc_available = false;
    std::uint64_t connection_ready_count = 0;
    std::uint64_t disconnect_count = 0;
    std::uint64_t reconnect_attempt_count = 0;
    AccessNacosSubscriptionStatus subscriptions;
    AccessNacosRegistrationStatus registrations;
};

// Every event maps to one predeclared Prometheus series. Runtime identifiers,
// service names, clusters, endpoints, and error text cannot become labels.
enum class AccessDiscoveryMetricEvent : std::uint8_t {
    ServiceUpdateChanged,
    ServiceUpdateUnchanged,
    ServiceRetiredReleased,
    ServiceRetiredSubscriptionClosed,
    ServiceRetiredShutdown,
    SelectorAcquireSucceeded,
    SelectorAcquireInvalidArgument,
    SelectorAcquireShutdown,
    SelectorAcquireAuthenticationUnavailable,
    SelectorAcquireTransport,
    SelectorAcquireGrpcStatus,
    SelectorAcquireProtocol,
    SelectorAcquireServer,
    SelectorAcquireResponseTooLarge,
    Count,
};

// One ready ServiceDiscovery state contributes one aggregate. Endpoint and
// cluster counts are sums across ready states, not globally unique identities.
struct AccessDiscoveryServiceAggregate {
    bool ready = false;
    std::uint64_t selectable_endpoints = 0;
    std::uint64_t logical_clusters = 0;
};

struct AccessDiscoveryStatus {
    std::array<AccessNacosLifecycleState, static_cast<std::size_t>(AccessNacosComponent::Count)> lifecycle{};
    std::array<AccessNacosTransportStatus, static_cast<std::size_t>(AccessNacosTransportComponent::Count)> transport{};
    std::uint64_t ready_services = 0;
    std::uint64_t selectable_endpoints = 0;
    std::uint64_t logical_clusters = 0;
    std::uint64_t selector_leases = 0;
};

struct AccessDiscoveryMetricsObserver {
    using EventFunction = void (*)(void *context, AccessDiscoveryMetricEvent event) noexcept;
    using ServiceTransitionFunction = void (*)(void *context, AccessDiscoveryMetricEvent event,
                                               const AccessDiscoveryServiceAggregate &before,
                                               const AccessDiscoveryServiceAggregate &after) noexcept;
    using LifecycleFunction = void (*)(void *context, AccessNacosComponent component,
                                       AccessNacosLifecycleState state) noexcept;
    using TransportFunction = void (*)(void *context, AccessNacosTransportComponent component,
                                       const AccessNacosTransportStatus &status) noexcept;
    using LeaseFunction = void (*)(void *context, bool acquired) noexcept;

    void record_event(AccessDiscoveryMetricEvent event) const noexcept {
        if (on_event != nullptr) {
            on_event(context, event);
        }
    }

    void transition_service(AccessDiscoveryMetricEvent event, const AccessDiscoveryServiceAggregate &before,
                            const AccessDiscoveryServiceAggregate &after) const noexcept {
        if (on_service_transition != nullptr) {
            on_service_transition(context, event, before, after);
        }
    }

    void set_lifecycle(AccessNacosComponent component, AccessNacosLifecycleState state) const noexcept {
        if (on_lifecycle != nullptr) {
            on_lifecycle(context, component, state);
        }
    }

    void update_transport(AccessNacosTransportComponent component,
                          const AccessNacosTransportStatus &status) const noexcept {
        if (on_transport != nullptr) {
            on_transport(context, component, status);
        }
    }

    void selector_lease(bool acquired) const noexcept {
        if (on_selector_lease != nullptr) {
            on_selector_lease(context, acquired);
        }
    }

    void *context = nullptr;
    EventFunction on_event = nullptr;
    ServiceTransitionFunction on_service_transition = nullptr;
    LifecycleFunction on_lifecycle = nullptr;
    TransportFunction on_transport = nullptr;
    LeaseFunction on_selector_lease = nullptr;
};

// The Nacos EventLoop is the sole writer for lifecycle and service aggregates.
// Selector lease destruction may run on any request worker and therefore uses
// an independent atomic gauge. Metrics workers only take bounded snapshots.
class AccessDiscoveryMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessDiscoveryMetrics(event::EventLoop &owner) noexcept;

    [[nodiscard]] AccessDiscoveryMetricsObserver observer() noexcept;
    [[nodiscard]] AccessDiscoveryStatus status() const noexcept;
    void append_prometheus(std::string &output) const;

private:
    static constexpr std::size_t kComponentCount = static_cast<std::size_t>(AccessNacosComponent::Count);
    static constexpr std::size_t kTransportCount = static_cast<std::size_t>(AccessNacosTransportComponent::Count);
    static constexpr std::size_t kEventCount = static_cast<std::size_t>(AccessDiscoveryMetricEvent::Count);

    struct Snapshot {
        std::uint64_t ready_services = 0;
        std::uint64_t selectable_endpoints = 0;
        std::uint64_t logical_clusters = 0;
        std::array<AccessNacosTransportStatus, kTransportCount> transport{};
    };

    static void observe_event(void *context, AccessDiscoveryMetricEvent event) noexcept;
    static void observe_service_transition(void *context, AccessDiscoveryMetricEvent event,
                                           const AccessDiscoveryServiceAggregate &before,
                                           const AccessDiscoveryServiceAggregate &after) noexcept;
    static void observe_lifecycle(void *context, AccessNacosComponent component,
                                  AccessNacosLifecycleState state) noexcept;
    static void observe_transport(void *context, AccessNacosTransportComponent component,
                                  const AccessNacosTransportStatus &status) noexcept;
    static void observe_selector_lease(void *context, bool acquired) noexcept;

    void record_event(AccessDiscoveryMetricEvent event) noexcept;
    void transition_service(AccessDiscoveryMetricEvent event, const AccessDiscoveryServiceAggregate &before,
                            const AccessDiscoveryServiceAggregate &after) noexcept;
    void set_lifecycle(AccessNacosComponent component, AccessNacosLifecycleState state) noexcept;
    void update_transport(AccessNacosTransportComponent component, const AccessNacosTransportStatus &status) noexcept;
    void selector_lease(bool acquired) noexcept;
    void begin_update() noexcept;
    void finish_update() noexcept;
    [[nodiscard]] Snapshot load_snapshot() const noexcept;

    event::EventLoop *owner_ = nullptr;
    std::array<std::atomic<std::uint64_t>, kEventCount> events_{};
    std::array<std::atomic<std::uint8_t>, kComponentCount> lifecycle_{};
    std::array<std::atomic<std::uint8_t>, kTransportCount> transport_phase_{};
    std::array<std::atomic<std::uint8_t>, kTransportCount> transport_failure_{};
    std::array<std::atomic<bool>, kTransportCount> rpc_available_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> connection_ready_count_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> disconnect_count_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> reconnect_attempt_count_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> subscription_active_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> subscription_pending_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> subscription_registered_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> subscription_synchronized_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> registration_active_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> registration_pending_{};
    std::array<std::atomic<std::uint64_t>, kTransportCount> registration_registered_{};
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> ready_services_{0};
    std::atomic<std::uint64_t> selectable_endpoints_{0};
    std::atomic<std::uint64_t> logical_clusters_{0};
    std::atomic<std::uint64_t> selector_leases_{0};
    std::uint64_t writer_ready_services_ = 0;
    std::uint64_t writer_selectable_endpoints_ = 0;
    std::uint64_t writer_logical_clusters_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_DISCOVERY_METRICS_H
