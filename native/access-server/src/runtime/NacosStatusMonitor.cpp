#include "NacosStatusMonitor.h"

#include <memory>
#include <utility>

#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>
#include <fiber/nacos/NacosServiceStatus.h>

namespace fiber::access_server {
namespace {

AccessNacosTransportPhase transport_phase(nacos::NacosServicePhase phase) noexcept {
    switch (phase) {
        case nacos::NacosServicePhase::Created:
            return AccessNacosTransportPhase::Created;
        case nacos::NacosServicePhase::Connecting:
            return AccessNacosTransportPhase::Connecting;
        case nacos::NacosServicePhase::Ready:
            return AccessNacosTransportPhase::Ready;
        case nacos::NacosServicePhase::ReconnectBackoff:
            return AccessNacosTransportPhase::ReconnectBackoff;
        case nacos::NacosServicePhase::Stopping:
            return AccessNacosTransportPhase::Stopping;
        case nacos::NacosServicePhase::Stopped:
            return AccessNacosTransportPhase::Stopped;
    }
    FIBER_PANIC("unknown Nacos service phase");
}

AccessNacosTransportFailure transport_failure(nacos::NacosServiceFailureCategory failure) noexcept {
    switch (failure) {
        case nacos::NacosServiceFailureCategory::None:
            return AccessNacosTransportFailure::None;
        case nacos::NacosServiceFailureCategory::AuthenticationUnavailable:
            return AccessNacosTransportFailure::AuthenticationUnavailable;
        case nacos::NacosServiceFailureCategory::Transport:
            return AccessNacosTransportFailure::Transport;
        case nacos::NacosServiceFailureCategory::GrpcStatus:
            return AccessNacosTransportFailure::GrpcStatus;
        case nacos::NacosServiceFailureCategory::Protocol:
            return AccessNacosTransportFailure::Protocol;
        case nacos::NacosServiceFailureCategory::Server:
            return AccessNacosTransportFailure::Server;
        case nacos::NacosServiceFailureCategory::Shutdown:
            return AccessNacosTransportFailure::Shutdown;
    }
    FIBER_PANIC("unknown Nacos service failure category");
}

AccessNacosTransportStatus transport_status(const nacos::NacosConnectionStatus &connection,
                                            const nacos::NacosSubscriptionSummary &subscriptions,
                                            const nacos::NacosRegistrationSummary &registrations = {}) noexcept {
    return AccessNacosTransportStatus{
            .phase = transport_phase(connection.phase),
            .failure = transport_failure(connection.failure),
            .rpc_available = connection.rpc_available,
            .connection_ready_count = connection.connection_ready_count,
            .disconnect_count = connection.disconnect_count,
            .reconnect_attempt_count = connection.reconnect_attempt_count,
            .subscriptions =
                    {
                            .active = subscriptions.active_count,
                            .pending = subscriptions.pending_count,
                            .registered = subscriptions.registered_count,
                            .synchronized = subscriptions.synchronized_count,
                    },
            .registrations =
                    {
                            .active = registrations.active_count,
                            .pending = registrations.pending_count,
                            .registered = registrations.registered_count,
                    },
    };
}

} // namespace

NacosStatusMonitor::NacosStatusMonitor(event::EventLoop &owner, nacos::ConfigService &config_service,
                                       nacos::NamingService &naming_service,
                                       AccessDiscoveryMetricsObserver metrics) noexcept :
    owner_(&owner), config_service_(&config_service), naming_service_(&naming_service), metrics_(metrics) {
    stopping_publisher_ = stopping_.acquire_publisher();
    FIBER_ASSERT(stopping_publisher_.has_value());
}

NacosStatusMonitor::~NacosStatusMonitor() noexcept {
    FIBER_ASSERT(state_ == State::Created || state_ == State::Stopped);
    FIBER_ASSERT(tasks_.empty());
}

void NacosStatusMonitor::start() {
    FIBER_ASSERT(owner_->in_loop());
    FIBER_ASSERT(state_ == State::Created);

    auto config = config_service_->subscribe_status();
    auto config_snapshot = config.current();
    FIBER_ASSERT(config_snapshot.value);
    metrics_.update_transport(
            AccessNacosTransportComponent::ConfigService,
            transport_status(config_snapshot.value->connection, config_snapshot.value->subscriptions));

    auto naming = naming_service_->subscribe_status();
    auto naming_snapshot = naming.current();
    FIBER_ASSERT(naming_snapshot.value);
    metrics_.update_transport(AccessNacosTransportComponent::NamingService,
                              transport_status(naming_snapshot.value->connection, naming_snapshot.value->subscriptions,
                                               naming_snapshot.value->registrations));

    state_ = State::Running;
    tasks_.add(2);
    async::spawn([this, subscriber = std::move(config), version = config_snapshot.version]() mutable {
        return watch_config(std::move(subscriber), version);
    });
    async::spawn([this, subscriber = std::move(naming), version = naming_snapshot.version]() mutable {
        return watch_naming(std::move(subscriber), version);
    });
}

async::DetachedTask NacosStatusMonitor::watch_config(nacos::ConfigService::StatusSubscriber subscriber,
                                                     std::uint64_t version) noexcept {
    auto stopping = stopping_.subscribe();
    auto stop_snapshot = stopping.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            const auto final = subscriber.current();
            FIBER_ASSERT(final.value);
            metrics_.update_transport(AccessNacosTransportComponent::ConfigService,
                                      transport_status(final.value->connection, final.value->subscriptions));
            break;
        }
        auto next = co_await async::when_any(
                [&subscriber, version]() { return subscriber.next(version); },
                [&stopping, stop_version = stop_snapshot.version]() { return stopping.next(stop_version); });
        if (next.is<0>()) {
            auto snapshot = std::move(next).get<0>();
            version = snapshot.version;
            FIBER_ASSERT(snapshot.value);
            metrics_.update_transport(AccessNacosTransportComponent::ConfigService,
                                      transport_status(snapshot.value->connection, snapshot.value->subscriptions));
        } else {
            stop_snapshot = std::move(next).get<1>();
        }
    }
    tasks_.done();
}

async::DetachedTask NacosStatusMonitor::watch_naming(nacos::NamingService::StatusSubscriber subscriber,
                                                     std::uint64_t version) noexcept {
    auto stopping = stopping_.subscribe();
    auto stop_snapshot = stopping.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            const auto final = subscriber.current();
            FIBER_ASSERT(final.value);
            metrics_.update_transport(
                    AccessNacosTransportComponent::NamingService,
                    transport_status(final.value->connection, final.value->subscriptions, final.value->registrations));
            break;
        }
        auto next = co_await async::when_any(
                [&subscriber, version]() { return subscriber.next(version); },
                [&stopping, stop_version = stop_snapshot.version]() { return stopping.next(stop_version); });
        if (next.is<0>()) {
            auto snapshot = std::move(next).get<0>();
            version = snapshot.version;
            FIBER_ASSERT(snapshot.value);
            metrics_.update_transport(AccessNacosTransportComponent::NamingService,
                                      transport_status(snapshot.value->connection, snapshot.value->subscriptions,
                                                       snapshot.value->registrations));
        } else {
            stop_snapshot = std::move(next).get<1>();
        }
    }
    tasks_.done();
}

async::Task<void> NacosStatusMonitor::shutdown() noexcept {
    FIBER_ASSERT(owner_->in_loop());
    if (state_ == State::Stopped) {
        co_return;
    }
    if (state_ == State::Created) {
        state_ = State::Stopped;
        co_return;
    }
    FIBER_ASSERT(stopping_publisher_);
    stopping_publisher_->publish(true);
    co_await tasks_.join();
    stopping_publisher_.reset();
    state_ = State::Stopped;
}

} // namespace fiber::access_server
