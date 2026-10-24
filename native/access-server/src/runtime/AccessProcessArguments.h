#ifndef FIBER_ACCESS_SERVER_ACCESS_PROCESS_ARGUMENTS_H
#define FIBER_ACCESS_SERVER_ACCESS_PROCESS_ARGUMENTS_H

#include <chrono>
#include <span>
#include <string>
#include <string_view>

#include <fiber/common/IoError.h>

namespace fiber::access_server {

// access-server [config.env] [--boss-fd=<n>]. Without --boss-fd the process runs
// standalone; with it the inherited descriptor is the boss channel.
struct AccessProcessArguments {
    std::string config_path = "access-server.env";
    int boss_fd = -1;
};

[[nodiscard]] common::IoResult<AccessProcessArguments>
parse_access_process_arguments(std::span<const std::string_view> arguments);

// access-server-boss --socket=<path> --server=<path> --config=<path>
//                    [--prewarm-timeout-millis=<1..3600000>]
struct AccessBossArguments {
    std::string socket_path;
    std::string server_path;
    std::string config_path;
    std::chrono::milliseconds prewarm_timeout{120000};
};

[[nodiscard]] common::IoResult<AccessBossArguments>
parse_access_boss_arguments(std::span<const std::string_view> arguments);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_PROCESS_ARGUMENTS_H
