#include "AccessServerRuntimeError.h"

#include <utility>

namespace fiber::access_server {

AccessServerRuntimeError make_access_server_runtime_create_error(AccessServerRuntimeErrorCode code,
                                                                 nacos::NacosCreateError error) noexcept {
    return AccessServerRuntimeError{
            .code = code,
            .create_error = error.code,
    };
}

AccessServerRuntimeError make_access_server_runtime_io_error(AccessServerRuntimeErrorCode code, common::IoErr error,
                                                             std::string message) noexcept {
    return AccessServerRuntimeError{
            .code = code,
            .io_error = error,
            .message = std::move(message),
    };
}

std::string_view access_server_runtime_stage_name(AccessServerRuntimeErrorCode code) noexcept {
    switch (code) {
        case AccessServerRuntimeErrorCode::CreateNacosClient:
            return "create Nacos client";
        case AccessServerRuntimeErrorCode::CreateConfigService:
            return "create Nacos config service";
        case AccessServerRuntimeErrorCode::CreateNamingService:
            return "create Nacos naming service";
        case AccessServerRuntimeErrorCode::CreateCatClient:
            return "create CAT client";
        case AccessServerRuntimeErrorCode::LoadDnsConfiguration:
            return "load DNS resolver configuration";
        case AccessServerRuntimeErrorCode::InitializeUpstreamTls:
            return "initialize upstream TLS trust store";
        case AccessServerRuntimeErrorCode::AllocateRuntime:
            return "allocate access-server runtime";
        case AccessServerRuntimeErrorCode::InitializeWorkers:
            return "initialize HTTP worker resources";
        case AccessServerRuntimeErrorCode::StartNacosClient:
            return "start Nacos client";
        case AccessServerRuntimeErrorCode::StartConfigService:
            return "start Nacos config service";
        case AccessServerRuntimeErrorCode::StartNamingService:
            return "start Nacos naming service";
        case AccessServerRuntimeErrorCode::StartCatClient:
            return "start CAT client";
        case AccessServerRuntimeErrorCode::StartGrayWatcher:
            return "subscribe gray configuration";
        case AccessServerRuntimeErrorCode::StartTlsCertificateWatcher:
            return "subscribe TLS certificate configuration";
        case AccessServerRuntimeErrorCode::StartAccessWatcher:
            return "subscribe access configuration";
        case AccessServerRuntimeErrorCode::InitialConfigUnavailable:
            return "synchronize initial access configuration";
        case AccessServerRuntimeErrorCode::InitialConfigTimeout:
            return "wait for initial access configuration";
        case AccessServerRuntimeErrorCode::InitialTlsCertificateUnavailable:
            return "receive initial TLS certificate snapshot";
        case AccessServerRuntimeErrorCode::InitialTlsCertificateTimeout:
            return "wait for initial TLS certificate snapshot";
        case AccessServerRuntimeErrorCode::Bind:
            return "bind gateway listener";
        case AccessServerRuntimeErrorCode::BindMetrics:
            return "bind Prometheus listener";
    }
    return "start access-server";
}

} // namespace fiber::access_server
