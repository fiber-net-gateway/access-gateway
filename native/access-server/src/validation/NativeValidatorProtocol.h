#ifndef FIBER_ACCESS_SERVER_NATIVE_VALIDATOR_PROTOCOL_H
#define FIBER_ACCESS_SERVER_NATIVE_VALIDATOR_PROTOCOL_H

#include <cstddef>
#include <string>
#include <string_view>

namespace fiber::access_server {

inline constexpr std::size_t kNativeValidatorMaxProtocolInputBytes = 8U * 1024U * 1024U;
inline constexpr int kNativeValidatorContractVersion = 1;

// Processes one complete versioned JSON request. The returned JSON never
// includes the input payload or project configuration values beyond bounded
// validation field paths.
[[nodiscard]] std::string process_native_validator_request(std::string_view input);

[[nodiscard]] std::string native_validator_input_too_large_response();

// Returns the versioned, machine-readable limits enforced by both the runtime
// and this validator. It contains no deployment configuration or secrets.
[[nodiscard]] std::string native_validator_config_limits_response();

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_NATIVE_VALIDATOR_PROTOCOL_H
