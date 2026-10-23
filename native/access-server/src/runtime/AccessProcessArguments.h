#ifndef FIBER_ACCESS_SERVER_ACCESS_PROCESS_ARGUMENTS_H
#define FIBER_ACCESS_SERVER_ACCESS_PROCESS_ARGUMENTS_H

#include <chrono>
#include <span>
#include <string>
#include <string_view>

#include <fiber/common/IoError.h>

namespace fiber::access_server {

struct AccessProcessArguments {
    std::string config_path = "access-server.env";
    std::string control_directory;
    std::chrono::milliseconds prewarm_timeout{120000};
    bool prewarm = false;
};

[[nodiscard]] common::IoResult<AccessProcessArguments>
parse_access_process_arguments(std::span<const std::string_view> arguments);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_PROCESS_ARGUMENTS_H
