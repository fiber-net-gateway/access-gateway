#include "AccessInstanceRegistration.h"

#include <chrono>
#include <utility>

#include <fiber/async/Timeout.h>
#include <fiber/common/Assert.h>
#include <fiber/net/LocalAddress.h>

namespace fiber::access_server {

AccessInstanceRegistration::AccessInstanceRegistration(event::EventLoop &loop, nacos::NamingService &naming_service,
                                                       AccessInstanceRegistrationOptions options) noexcept :
    loop_(&loop), naming_service_(&naming_service), options_(std::move(options)) {}

AccessInstanceRegistration::~AccessInstanceRegistration() noexcept { FIBER_ASSERT(!registration_); }

async::Task<std::expected<void, AccessServerRuntimeError>>
AccessInstanceRegistration::start(net::IpAddress bound_address, std::uint16_t port) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!registration_);

    net::IpAddress advertised = bound_address;
    if (options_.advertise_address) {
        advertised = *options_.advertise_address;
    } else if (advertised.is_unspecified()) {
        auto detected = net::detect_local_ipv4();
        if (!detected) {
            co_return std::unexpected(make_access_server_runtime_io_error(
                    AccessServerRuntimeErrorCode::RegisterNacosInstance, common::IoErr::AddrNotAvailable,
                    "no usable advertise address is available"));
        }
        advertised = detected->address;
    }

    nacos::Instance instance{
            .ip = advertised.to_string(),
            .port = port,
            .cluster_name = options_.cluster_name,
            .service_name = options_.service_name,
            .metadata = options_.metadata,
    };
    auto created = naming_service_->registry(options_.service_name, options_.group, std::move(instance));
    if (!created) {
        const common::IoErr io_error =
                created.error().io_error == common::IoErr::None ? common::IoErr::Invalid : created.error().io_error;
        co_return std::unexpected(
                make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::RegisterNacosInstance, io_error,
                                                    "Nacos rejected the instance registration request"));
    }
    registration_.emplace(std::move(*created));

    auto status = registration_->subscribe_status();
    auto snapshot = status.current();
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(options_.timeout);
    const auto deadline = event::EventLoop::current().now() + timeout;
    while (!snapshot.value || snapshot.value->state == nacos::RegistrationState::Pending) {
        const auto now = event::EventLoop::current().now();
        if (now >= deadline) {
            close();
            co_return std::unexpected(make_access_server_runtime_io_error(
                    AccessServerRuntimeErrorCode::WaitNacosRegistration, common::IoErr::TimedOut,
                    "Nacos instance registration did not become active before the startup deadline"));
        }
        auto next = co_await async::timeout_for(
                [&status, version = snapshot.version]() { return status.next(version); }, deadline - now);
        if (!next) {
            close();
            co_return std::unexpected(make_access_server_runtime_io_error(
                    AccessServerRuntimeErrorCode::WaitNacosRegistration, common::IoErr::TimedOut,
                    "Nacos instance registration did not become active before the startup deadline"));
        }
        snapshot = std::move(*next);
    }
    if (snapshot.value->state != nacos::RegistrationState::Registered) {
        const common::IoErr io_error = snapshot.value->error && snapshot.value->error->io_error != common::IoErr::None
                                               ? snapshot.value->error->io_error
                                               : common::IoErr::NotConnected;
        close();
        co_return std::unexpected(
                make_access_server_runtime_io_error(AccessServerRuntimeErrorCode::WaitNacosRegistration, io_error,
                                                    "Nacos instance registration failed before serving started"));
    }
    co_return std::expected<void, AccessServerRuntimeError>{};
}

void AccessInstanceRegistration::close() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!registration_) {
        return;
    }
    registration_->close();
    registration_.reset();
}

} // namespace fiber::access_server
