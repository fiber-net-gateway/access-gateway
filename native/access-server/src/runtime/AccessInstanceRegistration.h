#ifndef FIBER_ACCESS_SERVER_ACCESS_INSTANCE_REGISTRATION_H
#define FIBER_ACCESS_SERVER_ACCESS_INSTANCE_REGISTRATION_H

#include "AccessServerRuntimeError.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/NamingService.h>
#include <fiber/net/IpAddress.h>

namespace fiber::access_server {

struct AccessInstanceRegistrationOptions {
    std::string service_name = "unified-access-server";
    std::string group = "DEFAULT_GROUP";
    std::string cluster_name = "DEFAULT";
    std::optional<net::IpAddress> advertise_address;
    std::chrono::milliseconds timeout{10000};
    std::vector<nacos::NamingMetadataEntry> metadata;
};

class AccessInstanceRegistration final : public common::NonCopyable, public common::NonMovable {
public:
    AccessInstanceRegistration(event::EventLoop &loop, nacos::NamingService &naming_service,
                               AccessInstanceRegistrationOptions options) noexcept;
    ~AccessInstanceRegistration() noexcept;

    [[nodiscard]] async::Task<std::expected<void, AccessServerRuntimeError>> start(net::IpAddress bound_address,
                                                                                   std::uint16_t port) noexcept;
    void close() noexcept;

    [[nodiscard]] bool active() const noexcept { return registration_.has_value(); }

private:
    event::EventLoop *loop_ = nullptr;
    nacos::NamingService *naming_service_ = nullptr;
    AccessInstanceRegistrationOptions options_;
    std::optional<nacos::InstanceRegistration> registration_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_INSTANCE_REGISTRATION_H
