#include "AccessDiscoveryMetrics.h"

#include <array>
#include <charconv>
#include <limits>
#include <string_view>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

struct EventDescription {
    std::string_view operation;
    std::string_view result;
    std::string_view reason;
};

constexpr std::array<EventDescription, static_cast<std::size_t>(AccessDiscoveryMetricEvent::Count)> kEvents{
        EventDescription{"update", "success", "changed"},
        EventDescription{"update", "ignored", "unchanged"},
        EventDescription{"retire", "retired", "released"},
        EventDescription{"retire", "retired", "subscription_closed"},
        EventDescription{"retire", "retired", "shutdown"},
        EventDescription{"acquire", "success", "acquired"},
        EventDescription{"acquire", "failure", "invalid_argument"},
        EventDescription{"acquire", "failure", "shutdown"},
        EventDescription{"acquire", "failure", "authentication_unavailable"},
        EventDescription{"acquire", "failure", "transport"},
        EventDescription{"acquire", "failure", "grpc_status"},
        EventDescription{"acquire", "failure", "protocol"},
        EventDescription{"acquire", "failure", "server"},
        EventDescription{"acquire", "failure", "response_too_large"},
};

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessNacosComponent::Count)> kComponents{
        "client",
        "config_service",
        "naming_service",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessNacosLifecycleState::Count)> kLifecycleStates{
        "created", "starting", "running", "failed", "stopping", "stopped",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessNacosTransportComponent::Count)>
        kTransportComponents{
                "config_service",
                "naming_service",
        };

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessNacosTransportPhase::Count)> kTransportPhases{
        "created", "connecting", "ready", "reconnect_backoff", "stopping", "stopped",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessNacosTransportFailure::Count)> kTransportFailures{
        "none", "authentication_unavailable", "transport", "grpc_status", "protocol", "server", "shutdown",
};

constexpr std::array<std::string_view, 3> kConnectionEvents{
        "ready",
        "disconnect",
        "reconnect_attempt",
};

constexpr std::array<std::string_view, 4> kSubscriptionStates{
        "active",
        "pending",
        "registered",
        "synchronized",
};

constexpr std::array<std::string_view, 3> kRegistrationStates{
        "active",
        "pending",
        "registered",
};

void append_unsigned(std::string &output, std::uint64_t value) {
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    FIBER_ASSERT(converted.ec == std::errc{});
    output.append(buffer.data(), converted.ptr);
}

void append_resource(std::string &output, std::string_view resource, std::uint64_t value) {
    output.append("access_server_discovery_resources{resource=\"");
    output.append(resource);
    output.append("\"} ");
    append_unsigned(output, value);
    output.push_back('\n');
}

std::uint64_t ready_value(const AccessDiscoveryServiceAggregate &aggregate) noexcept {
    return aggregate.ready ? 1U : 0U;
}

} // namespace

AccessDiscoveryMetrics::AccessDiscoveryMetrics(event::EventLoop &owner) noexcept : owner_(&owner) {
    for (auto &state: lifecycle_) {
        state.store(static_cast<std::uint8_t>(AccessNacosLifecycleState::Created), std::memory_order_relaxed);
    }
    for (auto &phase: transport_phase_) {
        phase.store(static_cast<std::uint8_t>(AccessNacosTransportPhase::Created), std::memory_order_relaxed);
    }
    for (auto &failure: transport_failure_) {
        failure.store(static_cast<std::uint8_t>(AccessNacosTransportFailure::None), std::memory_order_relaxed);
    }
}

AccessDiscoveryMetricsObserver AccessDiscoveryMetrics::observer() noexcept {
    return AccessDiscoveryMetricsObserver{
            .context = this,
            .on_event = &observe_event,
            .on_service_transition = &observe_service_transition,
            .on_lifecycle = &observe_lifecycle,
            .on_transport = &observe_transport,
            .on_selector_lease = &observe_selector_lease,
    };
}

void AccessDiscoveryMetrics::observe_event(void *context, AccessDiscoveryMetricEvent event) noexcept {
    static_cast<AccessDiscoveryMetrics *>(context)->record_event(event);
}

void AccessDiscoveryMetrics::observe_service_transition(void *context, AccessDiscoveryMetricEvent event,
                                                        const AccessDiscoveryServiceAggregate &before,
                                                        const AccessDiscoveryServiceAggregate &after) noexcept {
    static_cast<AccessDiscoveryMetrics *>(context)->transition_service(event, before, after);
}

void AccessDiscoveryMetrics::observe_lifecycle(void *context, AccessNacosComponent component,
                                               AccessNacosLifecycleState state) noexcept {
    static_cast<AccessDiscoveryMetrics *>(context)->set_lifecycle(component, state);
}

void AccessDiscoveryMetrics::observe_transport(void *context, AccessNacosTransportComponent component,
                                               const AccessNacosTransportStatus &status) noexcept {
    static_cast<AccessDiscoveryMetrics *>(context)->update_transport(component, status);
}

void AccessDiscoveryMetrics::observe_selector_lease(void *context, bool acquired) noexcept {
    static_cast<AccessDiscoveryMetrics *>(context)->selector_lease(acquired);
}

void AccessDiscoveryMetrics::record_event(AccessDiscoveryMetricEvent event) noexcept {
    const std::size_t index = static_cast<std::size_t>(event);
    FIBER_ASSERT(index < events_.size());
    events_[index].fetch_add(1, std::memory_order_relaxed);
}

void AccessDiscoveryMetrics::transition_service(AccessDiscoveryMetricEvent event,
                                                const AccessDiscoveryServiceAggregate &before,
                                                const AccessDiscoveryServiceAggregate &after) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    FIBER_ASSERT(before.ready || (before.selectable_endpoints == 0 && before.logical_clusters == 0));
    FIBER_ASSERT(after.ready || (after.selectable_endpoints == 0 && after.logical_clusters == 0));
    FIBER_ASSERT(writer_ready_services_ >= ready_value(before));
    FIBER_ASSERT(writer_selectable_endpoints_ >= before.selectable_endpoints);
    FIBER_ASSERT(writer_logical_clusters_ >= before.logical_clusters);

    record_event(event);
    begin_update();
    writer_ready_services_ = writer_ready_services_ - ready_value(before) + ready_value(after);
    writer_selectable_endpoints_ =
            writer_selectable_endpoints_ - before.selectable_endpoints + after.selectable_endpoints;
    writer_logical_clusters_ = writer_logical_clusters_ - before.logical_clusters + after.logical_clusters;
    ready_services_.store(writer_ready_services_, std::memory_order_relaxed);
    selectable_endpoints_.store(writer_selectable_endpoints_, std::memory_order_relaxed);
    logical_clusters_.store(writer_logical_clusters_, std::memory_order_relaxed);
    finish_update();
}

void AccessDiscoveryMetrics::set_lifecycle(AccessNacosComponent component, AccessNacosLifecycleState state) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    const std::size_t component_index = static_cast<std::size_t>(component);
    const std::size_t state_index = static_cast<std::size_t>(state);
    FIBER_ASSERT(component_index < lifecycle_.size());
    FIBER_ASSERT(state_index < kLifecycleStates.size());
    lifecycle_[component_index].store(static_cast<std::uint8_t>(state), std::memory_order_relaxed);
}

void AccessDiscoveryMetrics::update_transport(AccessNacosTransportComponent component,
                                              const AccessNacosTransportStatus &status) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    const std::size_t component_index = static_cast<std::size_t>(component);
    const std::size_t phase_index = static_cast<std::size_t>(status.phase);
    const std::size_t failure_index = static_cast<std::size_t>(status.failure);
    FIBER_ASSERT(component_index < kTransportCount);
    FIBER_ASSERT(phase_index < kTransportPhases.size());
    FIBER_ASSERT(failure_index < kTransportFailures.size());

    begin_update();
    transport_phase_[component_index].store(static_cast<std::uint8_t>(status.phase), std::memory_order_relaxed);
    transport_failure_[component_index].store(static_cast<std::uint8_t>(status.failure), std::memory_order_relaxed);
    rpc_available_[component_index].store(status.rpc_available, std::memory_order_relaxed);
    connection_ready_count_[component_index].store(status.connection_ready_count, std::memory_order_relaxed);
    disconnect_count_[component_index].store(status.disconnect_count, std::memory_order_relaxed);
    reconnect_attempt_count_[component_index].store(status.reconnect_attempt_count, std::memory_order_relaxed);
    subscription_active_[component_index].store(status.subscriptions.active, std::memory_order_relaxed);
    subscription_pending_[component_index].store(status.subscriptions.pending, std::memory_order_relaxed);
    subscription_registered_[component_index].store(status.subscriptions.registered, std::memory_order_relaxed);
    subscription_synchronized_[component_index].store(status.subscriptions.synchronized, std::memory_order_relaxed);
    registration_active_[component_index].store(status.registrations.active, std::memory_order_relaxed);
    registration_pending_[component_index].store(status.registrations.pending, std::memory_order_relaxed);
    registration_registered_[component_index].store(status.registrations.registered, std::memory_order_relaxed);
    finish_update();
}

void AccessDiscoveryMetrics::selector_lease(bool acquired) noexcept {
    if (acquired) {
        const std::uint64_t previous = selector_leases_.fetch_add(1, std::memory_order_relaxed);
        FIBER_ASSERT(previous != std::numeric_limits<std::uint64_t>::max());
        return;
    }
    const std::uint64_t previous = selector_leases_.fetch_sub(1, std::memory_order_relaxed);
    FIBER_ASSERT(previous > 0);
}

void AccessDiscoveryMetrics::begin_update() noexcept {
    const std::uint64_t previous = sequence_.fetch_add(1, std::memory_order_acq_rel);
    FIBER_ASSERT((previous & 1U) == 0);
}

void AccessDiscoveryMetrics::finish_update() noexcept {
    const std::uint64_t previous = sequence_.fetch_add(1, std::memory_order_release);
    FIBER_ASSERT((previous & 1U) != 0);
}

AccessDiscoveryMetrics::Snapshot AccessDiscoveryMetrics::load_snapshot() const noexcept {
    Snapshot snapshot;
    for (;;) {
        const std::uint64_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        snapshot.ready_services = ready_services_.load(std::memory_order_relaxed);
        snapshot.selectable_endpoints = selectable_endpoints_.load(std::memory_order_relaxed);
        snapshot.logical_clusters = logical_clusters_.load(std::memory_order_relaxed);
        for (std::size_t index = 0; index < kTransportCount; ++index) {
            AccessNacosTransportStatus &transport = snapshot.transport[index];
            const std::uint8_t phase = transport_phase_[index].load(std::memory_order_relaxed);
            const std::uint8_t failure = transport_failure_[index].load(std::memory_order_relaxed);
            FIBER_ASSERT(phase < static_cast<std::uint8_t>(AccessNacosTransportPhase::Count));
            FIBER_ASSERT(failure < static_cast<std::uint8_t>(AccessNacosTransportFailure::Count));
            transport.phase = static_cast<AccessNacosTransportPhase>(phase);
            transport.failure = static_cast<AccessNacosTransportFailure>(failure);
            transport.rpc_available = rpc_available_[index].load(std::memory_order_relaxed);
            transport.connection_ready_count = connection_ready_count_[index].load(std::memory_order_relaxed);
            transport.disconnect_count = disconnect_count_[index].load(std::memory_order_relaxed);
            transport.reconnect_attempt_count = reconnect_attempt_count_[index].load(std::memory_order_relaxed);
            transport.subscriptions.active = subscription_active_[index].load(std::memory_order_relaxed);
            transport.subscriptions.pending = subscription_pending_[index].load(std::memory_order_relaxed);
            transport.subscriptions.registered = subscription_registered_[index].load(std::memory_order_relaxed);
            transport.subscriptions.synchronized = subscription_synchronized_[index].load(std::memory_order_relaxed);
            transport.registrations.active = registration_active_[index].load(std::memory_order_relaxed);
            transport.registrations.pending = registration_pending_[index].load(std::memory_order_relaxed);
            transport.registrations.registered = registration_registered_[index].load(std::memory_order_relaxed);
        }
        if (sequence_.load(std::memory_order_acquire) == before) {
            return snapshot;
        }
    }
}

AccessDiscoveryStatus AccessDiscoveryMetrics::status() const noexcept {
    const Snapshot snapshot = load_snapshot();
    AccessDiscoveryStatus result{
            .transport = snapshot.transport,
            .ready_services = snapshot.ready_services,
            .selectable_endpoints = snapshot.selectable_endpoints,
            .logical_clusters = snapshot.logical_clusters,
            .selector_leases = selector_leases_.load(std::memory_order_relaxed),
    };
    for (std::size_t index = 0; index < lifecycle_.size(); ++index) {
        const std::uint8_t state = lifecycle_[index].load(std::memory_order_relaxed);
        FIBER_ASSERT(state < static_cast<std::uint8_t>(AccessNacosLifecycleState::Count));
        result.lifecycle[index] = static_cast<AccessNacosLifecycleState>(state);
    }
    return result;
}

void AccessDiscoveryMetrics::append_prometheus(std::string &output) const {
    const Snapshot snapshot = load_snapshot();
    output.reserve(output.size() + 8192);

    output.append("# HELP access_server_nacos_component_lifecycle Application "
                  "component lifecycle; not Nacos "
                  "transport connection status.\n");
    output.append("# TYPE access_server_nacos_component_lifecycle gauge\n");
    for (std::size_t component = 0; component < kComponents.size(); ++component) {
        const std::size_t active = lifecycle_[component].load(std::memory_order_relaxed);
        FIBER_ASSERT(active < kLifecycleStates.size());
        for (std::size_t state = 0; state < kLifecycleStates.size(); ++state) {
            output.append("access_server_nacos_component_lifecycle{component=\"");
            output.append(kComponents[component]);
            output.append("\",state=\"");
            output.append(kLifecycleStates[state]);
            output.append("\"} ");
            output.push_back(state == active ? '1' : '0');
            output.push_back('\n');
        }
    }

    output.append("# HELP access_server_nacos_transport_phase Current bounded Nacos service transport phase.\n");
    output.append("# TYPE access_server_nacos_transport_phase gauge\n");
    for (std::size_t component = 0; component < kTransportComponents.size(); ++component) {
        const std::size_t active = static_cast<std::size_t>(snapshot.transport[component].phase);
        FIBER_ASSERT(active < kTransportPhases.size());
        for (std::size_t phase = 0; phase < kTransportPhases.size(); ++phase) {
            output.append("access_server_nacos_transport_phase{component=\"");
            output.append(kTransportComponents[component]);
            output.append("\",phase=\"");
            output.append(kTransportPhases[phase]);
            output.append("\"} ");
            output.push_back(phase == active ? '1' : '0');
            output.push_back('\n');
        }
    }

    output.append("# HELP access_server_nacos_transport_failure Current bounded Nacos failure category; resets "
                  "to none on Ready.\n");
    output.append("# TYPE access_server_nacos_transport_failure gauge\n");
    for (std::size_t component = 0; component < kTransportComponents.size(); ++component) {
        const std::size_t active = static_cast<std::size_t>(snapshot.transport[component].failure);
        FIBER_ASSERT(active < kTransportFailures.size());
        for (std::size_t failure = 0; failure < kTransportFailures.size(); ++failure) {
            output.append("access_server_nacos_transport_failure{component=\"");
            output.append(kTransportComponents[component]);
            output.append("\",category=\"");
            output.append(kTransportFailures[failure]);
            output.append("\"} ");
            output.push_back(failure == active ? '1' : '0');
            output.push_back('\n');
        }
    }

    output.append("# HELP access_server_nacos_rpc_available Whether the current Nacos service RPC is available.\n");
    output.append("# TYPE access_server_nacos_rpc_available gauge\n");
    for (std::size_t component = 0; component < kTransportComponents.size(); ++component) {
        output.append("access_server_nacos_rpc_available{component=\"");
        output.append(kTransportComponents[component]);
        output.append("\"} ");
        output.push_back(snapshot.transport[component].rpc_available ? '1' : '0');
        output.push_back('\n');
    }

    output.append("# HELP access_server_nacos_connection_events_total Bounded Nacos connection transition "
                  "counters supplied by Fiber.\n");
    output.append("# TYPE access_server_nacos_connection_events_total counter\n");
    for (std::size_t component = 0; component < kTransportComponents.size(); ++component) {
        const AccessNacosTransportStatus &transport = snapshot.transport[component];
        const std::array<std::uint64_t, 3> values{
                transport.connection_ready_count,
                transport.disconnect_count,
                transport.reconnect_attempt_count,
        };
        for (std::size_t event = 0; event < kConnectionEvents.size(); ++event) {
            output.append("access_server_nacos_connection_events_total{component=\"");
            output.append(kTransportComponents[component]);
            output.append("\",event=\"");
            output.append(kConnectionEvents[event]);
            output.append("\"} ");
            append_unsigned(output, values[event]);
            output.push_back('\n');
        }
    }

    output.append("# HELP access_server_nacos_subscriptions Aggregate logical Nacos subscription states.\n");
    output.append("# TYPE access_server_nacos_subscriptions gauge\n");
    for (std::size_t component = 0; component < kTransportComponents.size(); ++component) {
        const AccessNacosSubscriptionStatus &subscriptions = snapshot.transport[component].subscriptions;
        const std::array<std::uint64_t, 4> values{
                subscriptions.active,
                subscriptions.pending,
                subscriptions.registered,
                subscriptions.synchronized,
        };
        for (std::size_t state = 0; state < kSubscriptionStates.size(); ++state) {
            output.append("access_server_nacos_subscriptions{component=\"");
            output.append(kTransportComponents[component]);
            output.append("\",state=\"");
            output.append(kSubscriptionStates[state]);
            output.append("\"} ");
            append_unsigned(output, values[state]);
            output.push_back('\n');
        }
    }

    output.append("# HELP access_server_nacos_registrations Aggregate NamingService registration states.\n");
    output.append("# TYPE access_server_nacos_registrations gauge\n");
    const AccessNacosRegistrationStatus &registrations =
            snapshot.transport[static_cast<std::size_t>(AccessNacosTransportComponent::NamingService)].registrations;
    const std::array<std::uint64_t, 3> registration_values{
            registrations.active,
            registrations.pending,
            registrations.registered,
    };
    for (std::size_t state = 0; state < kRegistrationStates.size(); ++state) {
        output.append("access_server_nacos_registrations{state=\"");
        output.append(kRegistrationStates[state]);
        output.append("\"} ");
        append_unsigned(output, registration_values[state]);
        output.push_back('\n');
    }

    output.append("# HELP access_server_discovery_events_total Bounded service "
                  "discovery lifecycle outcomes.\n");
    output.append("# TYPE access_server_discovery_events_total counter\n");
    for (std::size_t i = 0; i < kEvents.size(); ++i) {
        const EventDescription &event = kEvents[i];
        output.append("access_server_discovery_events_total{operation=\"");
        output.append(event.operation);
        output.append("\",result=\"");
        output.append(event.result);
        output.append("\",reason=\"");
        output.append(event.reason);
        output.append("\"} ");
        append_unsigned(output, events_[i].load(std::memory_order_relaxed));
        output.push_back('\n');
    }

    output.append("# HELP access_server_discovery_resources Aggregate active "
                  "service discovery resources.\n");
    output.append("# TYPE access_server_discovery_resources gauge\n");
    append_resource(output, "ready_service", snapshot.ready_services);
    append_resource(output, "selectable_endpoint", snapshot.selectable_endpoints);
    append_resource(output, "logical_cluster", snapshot.logical_clusters);
    append_resource(output, "selector_lease", selector_leases_.load(std::memory_order_relaxed));
}

} // namespace fiber::access_server
