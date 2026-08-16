#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_ERROR_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_ERROR_H

#include <cstdint>
#include <string>
#include <string_view>

#include <fiber/common/IoError.h>
#include <fiber/nacos/NacosCreateError.h>

namespace fiber::access_server {

enum class AccessServerRuntimeErrorCode : std::uint8_t {
    CreateNacosClient,
    CreateConfigService,
    CreateNamingService,
    CreateCatClient,
    InitializeUpstreamTls,
    AllocateRuntime,
    InitializeWorkers,
    StartNacosClient,
    StartConfigService,
    StartNamingService,
    StartCatClient,
    StartGrayWatcher,
    StartTlsCertificateWatcher,
    StartAccessWatcher,
    InitialConfigUnavailable,
    InitialConfigTimeout,
    InitialTlsCertificateUnavailable,
    InitialTlsCertificateTimeout,
    Bind,
    BindMetrics,
};

struct AccessServerRuntimeError {
    AccessServerRuntimeErrorCode code = AccessServerRuntimeErrorCode::AllocateRuntime;
    common::IoErr io_error = common::IoErr::None;
    nacos::NacosCreateErrorCode create_error = nacos::NacosCreateErrorCode::InvalidState;
    std::string message;
};

[[nodiscard]] AccessServerRuntimeError make_access_server_runtime_create_error(AccessServerRuntimeErrorCode code,
                                                                               nacos::NacosCreateError error) noexcept;
[[nodiscard]] AccessServerRuntimeError make_access_server_runtime_io_error(AccessServerRuntimeErrorCode code,
                                                                           common::IoErr error,
                                                                           std::string message = {}) noexcept;
[[nodiscard]] std::string_view access_server_runtime_stage_name(AccessServerRuntimeErrorCode code) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_RUNTIME_ERROR_H
